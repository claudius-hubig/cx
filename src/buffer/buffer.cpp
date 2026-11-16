#include "buffer.hpp"
#include "byte_constants.hpp"
#include "modes/c/syntax.hpp"
#include "modes/dif/syntax.hpp"
#include "modes/jsn/syntax.hpp"
#include "modes/md/syntax.hpp"
#include "modes/py/syntax.hpp"
#include "modes/sh/syntax.hpp"
#include "modes/tml/syntax.hpp"
#include "modes/yml/syntax.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <span>
#include <variant>

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

// --- Position-explicit editing methods ---

BufferPoint Buffer::insertAt(uint32_t cp, BufferPoint at) {
  noteLinesEdited(at.line, 1, cp == '\n' ? 2 : 1);
  auto [n, enc] = encodeUtf8(cp);

  auto& line = mutLine(at.line);
  if (cp == '\n') {
    std::string tail(line, at.offset);
    line.resize(at.offset);

    std::size_t autoIndent = 0;
    if (_mode == BufferMode::Cpp || _mode == BufferMode::Jsn || _mode == BufferMode::Py) {
      while (autoIndent < line.size() && line[autoIndent] == ' ')
        ++autoIndent;
    }

    if (autoIndent > 0) {
      std::string indented;
      indented.reserve(autoIndent + tail.size());
      indented.append(autoIndent, ' ');
      indented.append(tail);
      _lines.insert(at.line + 1, makeLine(std::move(indented)));
    } else {
      _lines.insert(at.line + 1, makeLine(std::move(tail)));
    }
    at.line++;
    at.offset = autoIndent;
  } else {
    auto* p = reinterpret_cast<char const*>(enc.data());
    line.insert(at.offset, p, n);
    at.offset += n;
  }
  _modified = true;
  ++_docVersion;
  return at;
}

BufferPoint Buffer::deleteAt(BufferPoint at) {
  std::size_t lineSz = lineContent(at.line).size();
  if (at.offset < lineSz) {
    noteLinesEdited(at.line, 1, 1);
    auto& line = mutLine(at.line);
    auto end = nextClusterStart(line, at.offset); // a character with its marks
    line.erase(line.begin() + at.offset, line.begin() + end);
    _modified = true;
    ++_docVersion;
  } else if (at.line + 1 < _lines.size()) {
    noteLinesEdited(at.line, 2, 1);
    // Take the next line's content out of the store first so the
    // mutLine call below operates on a stable index.
    std::string nextContent = lineContent(at.line + 1);
    _lines.erase(at.line + 1);
    auto& line = mutLine(at.line);
    line.append(nextContent);
    _modified = true;
    ++_docVersion;
  }
  return at; // cursor doesn't move for delete-at
}

BufferPoint Buffer::deleteBefore(BufferPoint at) {
  if (at.offset > 0) {
    noteLinesEdited(at.line, 1, 1);
    auto& line = mutLine(at.line);
    auto end = at.offset;
    at.offset = prevClusterStart(line, at.offset); // a character with its marks
    line.erase(line.begin() + at.offset, line.begin() + end);
    _modified = true;
    ++_docVersion;
  } else if (at.line > 0) {
    noteLinesEdited(at.line - 1, 2, 1);
    std::string lineContentCopy = lineContent(at.line);
    _lines.erase(at.line);
    auto& prev = mutLine(at.line - 1);
    std::size_t newOffset = prev.size();
    prev.append(lineContentCopy);
    at.line--;
    at.offset = newOffset;
    _modified = true;
    ++_docVersion;
  }
  return at;
}

BufferPoint Buffer::insertText(std::string_view text, BufferPoint at) {
  if (text.empty())
    return at;

  std::size_t newlineCount = 0;
  for (char c : text)
    if (c == '\n')
      ++newlineCount;
  noteLinesEdited(at.line, 1, 1 + newlineCount);

  if (newlineCount == 0) {
    auto& line = mutLine(at.line);
    line.insert(at.offset, text.data(), text.size());
    at.offset += text.size();
    _modified = true;
    ++_docVersion;
    return at;
  }

  // The first piece goes onto the line at `at` (after cutting its tail
  // off); every later piece becomes a new line, all inserted in one go,
  // and the tail moves to the end of the last one.
  std::string tail(lineContent(at.line), at.offset);
  auto firstNl = text.find('\n');
  auto& first = mutLine(at.line);
  first.resize(at.offset);
  first.append(text.substr(0, firstNl));

  std::vector<LinePtr> added;
  added.reserve(newlineCount);
  for (auto i = firstNl + 1;;) {
    auto nl = text.find('\n', i);
    if (nl == std::string_view::npos) {
      std::string last(text.substr(i));
      at.offset = last.size();
      last += tail;
      added.push_back(makeLine(std::move(last)));
      break;
    }
    added.push_back(makeLine(text.substr(i, nl - i)));
    i = nl + 1;
  }
  auto const count = added.size();
  _lines.insert(at.line + 1, std::move(added));
  at.line += count;
  _modified = true;
  ++_docVersion;
  return at;
}

void Buffer::deleteRegion(BufferPoint from, BufferPoint to) {
  if (to < from)
    std::swap(from, to);
  if (from.line >= _lines.size())
    return;

  to.line = std::min(to.line, _lines.size() - 1);
  to.offset = std::min(to.offset, lineContent(to.line).size());
  from.offset = std::min(from.offset, lineContent(from.line).size());
  noteLinesEdited(from.line, to.line - from.line + 1, 1);

  if (from.line == to.line) {
    mutLine(from.line).erase(from.offset, to.offset - from.offset);
  } else {
    std::string toTail(lineContent(to.line), to.offset);
    auto& fromLine = mutLine(from.line);
    fromLine.resize(from.offset);
    fromLine.append(toTail);
    _lines.erase(from.line + 1, to.line + 1);
  }

  _modified = true;
  ++_docVersion;
}

// offsetForVisualCol, findVisualRow, lastVisualRowStart, findVisualRowCached
// are free functions declared in utf8.hpp.

bool Buffer::isBlankLine(std::size_t idx) const {
  if (idx >= _lines.size())
    return true;
  for (char c : *_lines[idx])
    if (!isWhitespace(c))
      return false;
  return true;
}

std::size_t Buffer::screenRow(ColIdx cols, BufferPoint point, ViewFrom viewFrom, bool wrap) const {
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

  if (!wrap)
    return point.line >= viewFrom.line.value ? point.line - viewFrom.line.value : 0;

  std::size_t row = 0;
  for (std::size_t lineIdx = viewFrom.line.value; lineIdx <= point.line && lineIdx < _lines.size();
       ++lineIdx) {
    auto& line = *_lines[lineIdx];
    std::size_t end = (lineIdx == point.line) ? point.offset : line.size();
    // For the viewport line, start from the sub-line wrap offset.
    std::size_t start =
        (lineIdx == viewFrom.line.value && wrap) ? viewFrom.byteOffset() : std::size_t{0};

    std::size_t col = 0;
    for (std::size_t i = start; i < end && i < line.size();) {
      auto info = charInfoAt(line.data() + i, line.size() - i);
      if (col > 0 && col + info.width > cw) {
        ++row;
        col = 0;
      }
      col += info.width;
      if (col >= cw) {
        col = 0;
        ++row;
      }
      i += info.len;
    }
    // If not the point's line, account for the newline (line break).
    if (lineIdx < point.line) {
      ++row;
    }
  }
  return row;
}

std::string Buffer::extractRegion(BufferPoint from, BufferPoint to) const {
  if (to < from)
    std::swap(from, to);
  if (from.line >= _lines.size())
    return {};

  // Rough size estimate to avoid repeated reallocations.
  std::size_t estimate = 0;
  auto it = _lines.iteratorAt(from.line);
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l, ++it) {
    auto const& line = **it;
    std::size_t start = (l == from.line) ? std::min(from.offset, line.size()) : 0;
    std::size_t end = (l == to.line) ? std::min(to.offset, line.size()) : line.size();
    // +1 for the inter-line '\n'. The emission loop only writes one
    // between adjacent lines, never after the last, so omit the +1
    // for the final line and avoid a 1-byte over-reservation.
    estimate += end - start + (l < to.line ? 1 : 0);
  }
  std::string result;
  result.reserve(estimate);
  it = _lines.iteratorAt(from.line);
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l, ++it) {
    auto const& line = **it;
    std::size_t start = (l == from.line) ? from.offset : 0;
    std::size_t end = (l == to.line) ? to.offset : line.size();
    start = std::min(start, line.size());
    end = std::min(end, line.size());
    result.append(line.begin() + start, line.begin() + end);
    if (l < to.line)
      result += '\n';
  }
  return result;
}

void Buffer::scanForConflicts() {
  _hasConflicts = false;
  for (auto const& linePtr : _lines) {
    if (isConflictMarker(*linePtr)) {
      _hasConflicts = true;
      return;
    }
  }
}

void Buffer::noteLinesEdited(std::size_t first, std::size_t oldCount, std::size_t newCount) {
  auto& valid = _synCacheValidUpto;
  auto& from = _synCacheTailFrom;
  auto& upto = _synCacheTailUpto;
  std::size_t const oldEnd = first + oldCount, newEnd = first + newCount;

  // Move the entries below the edit along with their lines, as far down
  // as the cache holds anything (the rest was never filled in). The entry
  // of the last edited line goes too: it holds the state in which the
  // first line below was tokenized, which is what the tail is checked
  // against. What it costs is one move per entry below the edit, and
  // only for an edit that changes the number of lines.
  std::size_t const keptEnd = std::max(valid, upto);
  bool const inStep = std::visit(
      [&](auto& entries) {
        if (entries.size() != _lines.size())
          return false; // never filled, or lines replaced behind its back
        auto const at = [&](std::size_t i) {
          return entries.begin() + static_cast<std::ptrdiff_t>(i);
        };
        if (newCount > oldCount) {
          entries.resize(entries.size() + (newCount - oldCount));
          if (oldEnd <= keptEnd)
            std::move_backward(at(oldEnd - 1), at(keptEnd), at(keptEnd + newCount - oldCount));
        } else if (newCount < oldCount) {
          if (oldEnd <= keptEnd)
            std::move(at(oldEnd - 1), at(keptEnd), at(newEnd - 1));
          entries.resize(entries.size() - (oldCount - newCount));
        }
        return true;
      },
      _synCache);

  if (!inStep) {
    from = upto = 0;
  } else if (from == upto) {
    // No tail yet: what was valid below the edit becomes one.
    from = upto = 0;
    if (oldEnd <= valid) {
      from = newEnd;
      upto = valid + newCount - oldCount;
    }
  } else if (oldEnd <= from) {
    // Above the tail, which moves with its lines.
    from = from + newCount - oldCount;
    upto = upto + newCount - oldCount;
  } else if (first >= upto) {
    // Below the tail, which is not affected.
  } else if (oldEnd <= upto) {
    // In the tail: what is below the edit remains.
    from = newEnd;
    upto = upto + newCount - oldCount;
  } else if (first > from) {
    // Over the end of the tail: what is above the edit remains.
    upto = first;
  } else {
    from = upto = 0;
  }
  valid = std::min(valid, first);
}

std::span<SyntaxToken const> Buffer::cachedLineTokens(std::size_t lineIdx) const {
  ensureSynCache(lineIdx + 1);
  return synTokens(lineIdx);
}

std::span<SyntaxToken const> Buffer::synTokens(std::size_t i) const {
  return std::visit(
      [i](auto const& entries) -> std::span<SyntaxToken const> { return entries[i].tokens; },
      _synCache);
}

void Buffer::ensureSynCache(std::size_t upToLine) const {
  // Modes without cross-line state ignore it.
  auto stateless = [](void (*tokenize)(std::string const&, std::vector<SyntaxToken>&)) {
    return [tokenize](std::string const& line, std::monostate&, std::vector<SyntaxToken>& out) {
      tokenize(line, out);
    };
  };
  if (_mode == BufferMode::Md)
    fillSynCache<MdSyntaxState>(upToLine, tokenizeMarkdownLine);
  else if (_mode == BufferMode::Jsn)
    fillSynCache<std::monostate>(upToLine, stateless(tokenizeJsonLine));
  else if (_mode == BufferMode::Dif)
    fillSynCache<std::monostate>(upToLine, stateless(tokenizeDiffLine));
  else if (_mode == BufferMode::Cpp)
    fillSynCache<CSyntaxState>(upToLine, tokenizeLine);
  else if (_mode == BufferMode::Sh)
    fillSynCache<ShSyntaxState>(upToLine, tokenizeShellLine);
  else if (_mode == BufferMode::Yml)
    fillSynCache<YmlSyntaxState>(upToLine, tokenizeYamlLine);
  else if (_mode == BufferMode::Tml)
    fillSynCache<TmlSyntaxState>(upToLine, tokenizeTomlLine);
  else if (_mode == BufferMode::Py)
    fillSynCache<PySyntaxState>(upToLine, tokenizePythonLine);
  else
    fillSynCache<std::monostate>(
        upToLine, stateless([](std::string const& line, std::vector<SyntaxToken>& out) {
          out.assign(line.size(), SyntaxToken::Normal);
        }));
}

template <class S, class Tokenize>
void Buffer::fillSynCache(std::size_t upToLine, Tokenize tokenize) const {
  using Entries = std::vector<LineSyntax<S>>;
  // A mode with another state type replaces the whole cache.
  auto* cache = std::get_if<Entries>(&_synCache);
  if (!cache) {
    cache = &_synCache.template emplace<Entries>();
    _synCacheValidUpto = 0;
    _synCacheTailFrom = _synCacheTailUpto = 0;
  }
  // Size it on first use, and again if the lines were replaced without
  // the cache being told (the read-only buffers rebuild theirs).
  if (cache->size() != _lines.size()) {
    cache->resize(_lines.size());
    if (_synCacheValidUpto > _lines.size())
      _synCacheValidUpto = 0;
    _synCacheTailFrom = _synCacheTailUpto = 0;
  }
  if (upToLine > _lines.size())
    upToLine = _lines.size();
  if (_synCacheValidUpto >= upToLine)
    return; // already covered

  // Start from the state at the end of the previous valid line.
  S state{};
  if (_synCacheValidUpto > 0)
    state = (*cache)[_synCacheValidUpto - 1].stateAfter;

  std::size_t reached = upToLine;
  for (std::size_t i = _synCacheValidUpto; i < upToLine; ++i) {
    auto const& line = *_lines[i];
    auto& entry = (*cache)[i];
    tokenize(line, state, entry.tokens);
    // From the last edited line on, the entry still has the state this
    // line used to end in. Ending in it again means the lines below get
    // what they got before, so their entries are valid as they stand.
    bool const converged =
        i + 1 >= _synCacheTailFrom && i < _synCacheTailUpto && state == entry.stateAfter;
    entry.stateAfter = state;

    if (_hasConflicts && isConflictMarker(line))
      entry.tokens.assign(line.size(), SyntaxToken::ConflictMarker);

    if (converged) {
      reached = std::max(reached, _synCacheTailUpto);
      i = _synCacheTailUpto - 1; // carry on below the tail, if asked to
      state = (*cache)[i].stateAfter;
      _synCacheTailFrom = _synCacheTailUpto = 0;
    }
  }
  _synCacheValidUpto = reached;
  // Stopped inside the tail without getting back into step: the entry of
  // the line just done now holds its new end state, not the one the next
  // line was tokenized with. That line is redone next time and compared
  // with its own entry, so the tail starts below it.
  if (_synCacheValidUpto >= _synCacheTailFrom)
    _synCacheTailFrom = _synCacheValidUpto + 1;
  if (_synCacheTailFrom >= _synCacheTailUpto)
    _synCacheTailFrom = _synCacheTailUpto = 0;
}

std::tuple<RowIdx, ColIdx> Buffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                          RenderParams const& params) const {
  auto const& point = params.point;
  auto const& viewFrom = params.viewFrom;
  auto const& mark = params.mark;
  auto const& highlight = params.highlight;
  bool showWhitespace = params.showWhitespace;
  bool wrap = params.wrap;
  bool focused = params.focused;
  std::size_t rowOffset = params.rowOffset;
  std::size_t colOffset = params.colOffset;
  std::size_t leftGutter = params.leftGutter;
  // Content occupies the panel between the left gutter (line numbers)
  // and the right gutter (wrap/scroll indicator). When the panel is too
  // narrow to fit both gutters plus content, fall back to no left
  // gutter so the buffer remains usable.
  if (leftGutter + 2 > cols.value)
    leftGutter = 0;
  std::size_t contentColOffset = colOffset + leftGutter;
  static constexpr auto kCtrlColor = ansi::CtrlColor;
  static constexpr auto kWsFg = "\x1b[38;2;140;60;60m";
  static constexpr auto kSpaceStyle = "\x1b[48;2;45;45;45m";
  static constexpr auto kTabWsStyle = "\x1b[48;2;60;60;60m";
  static constexpr auto kTabStyle = "\x1b[48;2;70;70;70m";
  static constexpr auto kNlStyle = "\x1b[48;2;35;35;35m";

  // Syntax highlighting foreground colors.
  static constexpr auto kSynKeyword = "\x1b[38;2;80;130;200m";   // blue
  static constexpr auto kSynComment = "\x1b[38;2;80;160;80m";    // green
  static constexpr auto kSynString = "\x1b[38;2;200;80;80m";     // red
  static constexpr auto kSynOperator = "\x1b[38;2;150;100;200m"; // purple
  static constexpr auto kSynNumber = "\x1b[38;2;100;200;200m";   // light turquoise
  static constexpr auto kSynPreproc = "\x1b[38;2;180;140;60m";   // amber
  static constexpr auto kSynVariable = "\x1b[38;2;210;150;90m";  // orange (shell $VAR/${...})
  static constexpr auto kSynReset = "\x1b[22;39m";               // bold off + default fg
  // Bracket match: turquoise foreground (no bold/underline).
  static constexpr auto kBracketMatch = "\x1b[38;2;60;170;170m";

  // Diff / conflict line background colors.
  static constexpr auto kDiffAddBg = "\x1b[48;2;30;60;30m";
  static constexpr auto kDiffRemoveBg = "\x1b[48;2;70;30;30m";
  static constexpr auto kDiffHunkFg = "\x1b[38;2;120;160;220m";
  static constexpr auto kConflictOursBg = "\x1b[48;2;40;50;80m";
  static constexpr auto kConflictDivBg = "\x1b[48;2;70;60;30m";
  static constexpr auto kConflictTheirsBg = "\x1b[48;2;50;70;40m";
  // Subtle current-line tint: dark blue-grey, distinct from the
  // whitespace-marker greys (45/60/70) so an enabled cursor-line
  // highlight is visually separate from any visible-whitespace styling.
  static constexpr auto kCurrentLineBg = "\x1b[48;2;38;38;48m";
  static constexpr auto kBgReset = "\x1b[49m";

  // Per-line background for diff/conflict lines.
  char const* curLineBg = nullptr;
  auto getLineBg = [&](std::span<SyntaxToken const> const& toks,
                       std::string const& ln) -> char const* {
    if (toks.empty())
      return nullptr;
    switch (toks[0]) {
    case SyntaxToken::DiffAdd:
      return kDiffAddBg;
    case SyntaxToken::DiffRemove:
      return kDiffRemoveBg;
    case SyntaxToken::DiffMeta:
      return ansi::ReverseVideo.data();
    case SyntaxToken::DiffHunk:
      return nullptr;
    case SyntaxToken::ConflictMarker:
      if (!ln.empty()) {
        if (ln[0] == '<')
          return kConflictOursBg;
        if (ln[0] == '=')
          return kConflictDivBg;
        if (ln[0] == '>')
          return kConflictTheirsBg;
      }
      return nullptr;
    default:
      return nullptr;
    }
  };

  // Row start moves the cursor to the first column of the content
  // area (i.e. past the left gutter). Callers emit the left gutter
  // separately via emitLeftGutter() so its ANSI styling never bleeds
  // into the content emission state.
  auto emitRowStart = [&](std::size_t r) {
    ansi::appendCursorTo(out, rowOffset + r, contentColOffset);
  };

  // Per-character rendering state: tracks which ANSI highlight modes are
  // currently active so we can emit transitions only when they change.
  struct RenderState {
    // Inverse-video region highlight (mark+point selection).
    bool regionActive = false;
    // Blue-background find highlight.
    bool highlightActive = false;
    // Last syntax token foreground color applied; Normal = no color active.
    SyntaxToken lastSynTok = SyntaxToken::Normal;
    // Turquoise bracket-match foreground color.
    bool bracketMatchActive = false;
  };

  // Emit one character/codepoint to `out` and reset `rs` when the emitted
  // sequence includes ansi::Reset (which clears all ANSI state). Folding the
  // reset here ensures rs stays in sync without a separate post-emit call.
  auto emitChar = [&](char const* data, CharInfo info, RenderState& rs) {
    auto u = static_cast<unsigned char>(*data);
    if (u < 0x80) {
      if (*data == '\t') {
        if (showWhitespace) {
          out += kTabWsStyle;
          out += kWsFg;
          out += "\xe2\x96\xa0\xe2\x96\xba";
        } else {
          out += kTabStyle;
          out += "  ";
        }
        out += ansi::Reset;
        rs = {};
        if (curLineBg)
          out += curLineBg;
      } else if (*data == ' ' && showWhitespace) {
        out += kSpaceStyle;
        out += kWsFg;
        out += "\xc2\xb7";
        out.append(data + 1, info.len - 1); // combining marks on the space
        out += ansi::Reset;
        rs = {};
        if (curLineBg)
          out += curLineBg;
      } else if (u == 0) {
        out += kCtrlColor;
        out += "\\0";
        out += ansi::Reset;
        rs = {};
        if (curLineBg)
          out += curLineBg;
      } else if (u == 0x7f) {
        out += kCtrlColor;
        out += "^?";
        out += ansi::Reset;
        rs = {};
        if (curLineBg)
          out += curLineBg;
      } else if (u < 0x20) {
        out += kCtrlColor;
        out += '^';
        out += static_cast<char>('@' + u);
        out += ansi::Reset;
        rs = {};
        if (curLineBg)
          out += curLineBg;
      } else {
        out.append(data, info.len); // printable, with any combining marks
      }
    } else if (info.len == 1) {
      // Invalid or lone continuation byte: render as "\xHH" so a broken
      // byte is visually distinct from the surrounding text rather than
      // looking like a stray pair of hex digits.
      out += kCtrlColor;
      auto nibble = [](unsigned char n) -> char { return n < 10 ? ('0' + n) : ('a' + n - 10); };
      out += "\\x";
      out += nibble(u >> 4);
      out += nibble(u & 0xf);
      out += ansi::Reset;
      rs = {};
      if (curLineBg)
        out += curLineBg;
    } else if (auto cp = decodeUtf8(data, info.len).first; isEscapedCodepoint(cp)) {
      // Invisible format character (ZWSP, ZWJ, BOM, bidi control, ...),
      // line/paragraph separator or C1 control: show it as "\\u200b" so
      // it can't hide in the text or reach the terminal as a control.
      out += kCtrlColor;
      out += codepointEscape(cp);
      out += ansi::Reset;
      rs = {};
      if (curLineBg)
        out += curLineBg;
    } else {
      // A cluster starting with a mark has nothing to draw it onto.
      if (isCombiningMark(cp))
        out += kOrphanMarkBase;
      out.append(data, info.len);
    }
  };

  // Content width excludes both gutters.
  std::size_t cw = contentWidth(cols);
  if (cw > leftGutter)
    cw -= leftGutter;
  else
    cw = 1;

  // Emit the right-side gutter character at column cols.value for a
  // given row. `highlight` marks the visual row containing the cursor.
  auto emitGutter = [&](std::size_t r, bool isCursorRow, char const* indicator) {
    ansi::appendCursorTo(out, rowOffset + r, colOffset + cols.value - 1);
    out += (isCursorRow && focused) ? ansi::Reset : ansi::ReverseVideo;
    out += indicator;
    out += ansi::Reset;
  };

  // Emit the left-side gutter (line numbers / blanks) at column
  // colOffset for the given row. `displayLine` is the 1-based physical
  // line number to show; pass 0 for blank rows (trailing fill or
  // wrapped continuation rows). Both branches reset SGR after the
  // emission so the content emitter can rely on a clean ANSI state.
  auto emitLeftGutter = [&](std::size_t r, std::size_t displayLine) {
    if (leftGutter == 0)
      return;
    ansi::appendCursorTo(out, rowOffset + r, colOffset);
    out += ansi::Dim;
    if (displayLine == 0) {
      out.append(leftGutter, ' ');
    } else {
      auto numStr = std::to_string(displayLine);
      // Right-align inside (leftGutter - 1) cols, then a trailing space.
      if (numStr.size() + 1 < leftGutter)
        out.append(leftGutter - 1 - numStr.size(), ' ');
      // If the number itself is wider than the gutter (shouldn't happen
      // — gutter width is computed from lineCount — but guard anyway),
      // truncate so we don't spill into the content area.
      if (numStr.size() + 1 > leftGutter)
        numStr.resize(leftGutter - 1);
      out += numStr;
      out += ' ';
    }
    out += ansi::DimOff;
  };

  // Region highlighting: compute normalized start/end from mark and point.
  bool hasRegion = mark.has_value();
  BufferPoint regionStart, regionEnd;
  if (hasRegion) {
    regionStart = std::min(mark.value(), point);
    regionEnd = std::max(mark.value(), point);
  }

  // Find highlight (light blue background).
  static constexpr auto kHighlightOn = "\x1b[48;2;60;100;160m";

  // Helper: is position {lineIdx, byteOffset} within [regionStart, regionEnd)?
  auto inRegion = [&](std::size_t lineIdx, std::size_t byteOffset) -> bool {
    if (!hasRegion)
      return false;
    BufferPoint p{lineIdx, byteOffset};
    return regionStart <= p && p < regionEnd;
  };

  auto inHighlight = [&](std::size_t lineIdx, std::size_t byteOffset) -> bool {
    if (!highlight)
      return false;
    BufferPoint p{lineIdx, byteOffset};
    return highlight->from <= p && p < highlight->to;
  };

  // Secondary cursors render as a reverse-video cell, so they mark the
  // character under the cursor instead of replacing it. The span is
  // sorted, so a binary search suffices.
  auto const& extraCursors = params.extraCursors;
  auto atExtraCursor = [&](std::size_t lineIdx, std::size_t byteOffset) -> bool {
    if (extraCursors.empty())
      return false;
    return std::binary_search(extraCursors.begin(), extraCursors.end(),
                              BufferPoint{lineIdx, byteOffset});
  };

  std::size_t cur_row = 0, cur_col = 0;
  bool pointFound = false;

  // Extract viewport offsets from viewFrom.
  std::size_t hScroll =
      (!wrap && std::holds_alternative<VisualCol>(viewFrom.col)) ? viewFrom.hScroll() : 0;
  std::size_t viewFromOffset =
      (wrap && std::holds_alternative<ByteOffset>(viewFrom.col)) ? viewFrom.byteOffset() : 0;

  // Syntax highlighting pre-computation.
  bool syntaxActive = _mode != BufferMode::Txt || _hasConflicts;
  std::optional<BufferPoint> matchBracket;
  std::span<SyntaxToken const> lineTokens;

  if (syntaxActive) {
    // Build the per-line cache up to just past the last potentially visible
    // line. This replaces the O(viewFrom) advanceBlockCommentState loop with
    // an incremental O(changed-lines) update.
    std::size_t lastVisible =
        std::min(viewFrom.line.value + static_cast<std::size_t>(rows.value) + 1, _lines.size());
    ensureSynCache(lastVisible);
    // Bracket matching re-uses the cache. For any line not yet tokenized
    // (e.g. a forward scan running past the viewport) the cache is
    // extended incrementally — a single line at a time.
    if (_bracketCache.point == point && _bracketCache.docVersion == _docVersion) {
      matchBracket = _bracketCache.result;
    } else {
      auto getTokens = [this](std::size_t i) { return cachedLineTokens(i); };
      matchBracket = (_mode == BufferMode::Jsn) ? findMatchingBracketJson(_lines, point, getTokens)
                                                : findMatchingBracket(_lines, point, getTokens);
      _bracketCache = {point, _docVersion, matchBracket};
    }
  }

  // Markdown-specific syntax highlight colors.
  static constexpr auto kSynHeading = "\x1b[1;38;2;80;130;200m"; // bold + blue
  static constexpr auto kSynLink = "\x1b[38;2;100;180;180m";     // teal

  auto syntaxFg = [](SyntaxToken t) -> char const* {
    switch (t) {
    case SyntaxToken::Keyword:
      return kSynKeyword;
    case SyntaxToken::Comment:
      return kSynComment;
    case SyntaxToken::String:
      return kSynString;
    case SyntaxToken::Operator:
      return kSynOperator;
    case SyntaxToken::Heading:
      return kSynHeading;
    case SyntaxToken::Bold:
      return ansi::Bold.data();
    case SyntaxToken::Link:
      return kSynLink;
    case SyntaxToken::Number:
      return kSynNumber;
    case SyntaxToken::Preprocessor:
      return kSynPreproc;
    case SyntaxToken::Variable:
      return kSynVariable;
    case SyntaxToken::DiffHunk:
      return kDiffHunkFg;
    case SyntaxToken::DiffAdd:
    case SyntaxToken::DiffRemove:
    case SyntaxToken::DiffMeta:
    case SyntaxToken::ConflictMarker:
      return nullptr;
    default:
      return nullptr; // brackets and Normal have no special color
    }
  };

  // Shared highlight/region/syntax toggle logic. Updates rs and emits ANSI
  // escapes when transitioning in/out of styled spans.
  auto updateStyle = [&](std::size_t lineIdx, std::size_t byteOff, RenderState& rs) {
    bool wantHl = inHighlight(lineIdx, byteOff);
    bool wantRegion = !wantHl && (inRegion(lineIdx, byteOff) || atExtraCursor(lineIdx, byteOff));
    if (wantHl && !rs.highlightActive) {
      if (rs.regionActive) {
        out += ansi::Reset;
        rs.regionActive = false;
        if (curLineBg)
          out += curLineBg;
      }
      out += kHighlightOn;
      rs.highlightActive = true;
    } else if (!wantHl && rs.highlightActive) {
      out += ansi::Reset;
      rs.highlightActive = false;
      if (curLineBg)
        out += curLineBg;
    }
    if (wantRegion && !rs.regionActive) {
      if (rs.lastSynTok != SyntaxToken::Normal) {
        out += kSynReset;
        rs.lastSynTok = SyntaxToken::Normal;
      }
      if (rs.bracketMatchActive) {
        out += ansi::DefaultFg;
        rs.bracketMatchActive = false;
      }
      out += ansi::ReverseVideo;
      rs.regionActive = true;
    } else if (!wantRegion && rs.regionActive) {
      out += ansi::Reset;
      rs.regionActive = false;
      if (curLineBg)
        out += curLineBg;
    }

    // Syntax foreground coloring (skip when region is active).
    if (syntaxActive && !rs.regionActive && !lineTokens.empty()) {
      SyntaxToken tok = (byteOff < lineTokens.size()) ? lineTokens[byteOff] : SyntaxToken::Normal;
      if (tok != rs.lastSynTok) {
        auto fg = syntaxFg(tok);
        if (fg)
          out += fg;
        else if (rs.lastSynTok != SyntaxToken::Normal)
          out += kSynReset;
        rs.lastSynTok = tok;
      }
      bool wantMatch =
          matchBracket && ((lineIdx == matchBracket->line && byteOff == matchBracket->offset) ||
                           (lineIdx == point.line && byteOff == point.offset));
      if (wantMatch && !rs.bracketMatchActive) {
        out += kBracketMatch;
        rs.bracketMatchActive = true;
      } else if (!wantMatch && rs.bracketMatchActive) {
        // Restore the current syntax foreground instead of defaulting to
        // white — otherwise the DefaultFg clobbers the color we just set.
        auto restoreFg = syntaxFg(rs.lastSynTok);
        out += restoreFg ? restoreFg : ansi::DefaultFg;
        rs.bracketMatchActive = false;
      }
    }
  };

  // End-of-line: highlight the newline position if region spans it, show
  // whitespace indicator, then reset active styles.
  auto emitLineEnd = [&](std::size_t lineIdx, std::size_t lineSize, std::size_t visCol,
                         std::size_t maxCol, RenderState& rs) {
    if (rs.bracketMatchActive) {
      out += ansi::DefaultFg;
      rs.bracketMatchActive = false;
    }
    if (rs.lastSynTok != SyntaxToken::Normal) {
      out += kSynReset;
      rs.lastSynTok = SyntaxToken::Normal;
    }
    if (hasRegion && lineIdx + 1 < _lines.size() && visCol < maxCol &&
        inRegion(lineIdx, lineSize)) {
      if (!rs.regionActive) {
        out += ansi::ReverseVideo;
        rs.regionActive = true;
      }
      out += ' ';
    }
    // A secondary cursor sitting at end-of-line has no character cell to
    // invert, so give it a reverse-video space of its own.
    if (visCol < maxCol && atExtraCursor(lineIdx, lineSize)) {
      if (!rs.regionActive) {
        out += ansi::ReverseVideo;
        rs.regionActive = true;
      }
      out += ' ';
    }
    if (rs.highlightActive) {
      out += ansi::Reset;
      rs.highlightActive = false;
      if (curLineBg)
        out += curLineBg;
    }
    if (rs.regionActive) {
      out += ansi::Reset;
      rs.regionActive = false;
      if (curLineBg)
        out += curLineBg;
    }
    if (showWhitespace && lineIdx + 1 < _lines.size() && visCol < maxCol &&
        !inRegion(lineIdx, lineSize)) {
      out += kNlStyle;
      out += kWsFg;
      out += "\xe2\x86\xb5";
      out += ansi::Reset;
      if (curLineBg)
        out += curLineBg;
    }
  };

  // Resolve per-line syntax tokens + background colour and emit the
  // background (if any). Shared between the wrap and no-wrap branches.
  // The current-line tint (params.highlightCurrentLine) only applies
  // when no diff/conflict background already claims the line — those
  // cues are more important than the cursor cue.
  auto setupLine = [&](std::size_t lineIdx, std::string const& line) {
    if (syntaxActive) {
      lineTokens = synTokens(lineIdx);
      curLineBg = getLineBg(lineTokens, line);
    } else {
      curLineBg = nullptr;
    }
    if (!curLineBg && params.highlightCurrentLine && lineIdx == point.line)
      curLineBg = kCurrentLineBg;
    if (curLineBg)
      out += curLineBg;
  };

  // Emit `EraseLine` framed by the current line background so the rest
  // of the row is painted in the line's diff/conflict colour rather
  // than reverting to the default. Used at every visual-row boundary.
  auto emitEraseLineWithBg = [&]() {
    if (curLineBg)
      out += curLineBg;
    out += ansi::EraseLine;
    if (curLineBg)
      out += kBgReset;
  };

  // Fill rows [startRow, rows.value) with empty content + gutter. Both
  // wrap and no-wrap modes use this as the trailing-blank-row teardown.
  // The left gutter (if any) gets a blank entry on every trailing row
  // so the dim numbers don't leak into the empty space below the file.
  auto fillTrailingRows = [&](std::size_t startRow) {
    for (std::size_t r = startRow; r < rows.value; ++r) {
      emitLeftGutter(r, 0);
      if (r > startRow)
        emitRowStart(r);
      out += ansi::EraseLine;
      emitGutter(r, false, " ");
    }
  };

  if (!wrap) {
    // No-wrap: each physical line is one screen row, horizontally scrolled.
    std::size_t row = 0;
    emitLeftGutter(0, viewFrom.line.value + 1);
    emitRowStart(0);

    for (std::size_t lineIdx = viewFrom.line.value; lineIdx < _lines.size() && row < rows.value;
         ++lineIdx) {
      auto const& line = *_lines[lineIdx];
      std::size_t vcol = 0;
      bool overflows = false; // content extends past visible area
      RenderState rs;

      setupLine(lineIdx, line);

      for (std::size_t i = 0; i < line.size();) {
        auto info = charInfoAt(line.data() + i, line.size() - i);

        if (lineIdx == point.line && i <= point.offset && point.offset < i + info.len &&
            !pointFound) {
          cur_row = row;
          cur_col = leftGutter + (vcol >= hScroll ? vcol - hScroll : 0);
          pointFound = true;
        }

        if (vcol + info.width <= hScroll) {
          // Before visible area.
          vcol += info.width;
          i += info.len;
          continue;
        }
        if (vcol + info.width > hScroll + cw) {
          // Past or overflows visible area.
          overflows = true;
          break;
        }

        updateStyle(lineIdx, i, rs);
        emitChar(line.data() + i, info, rs);
        vcol += info.width;
        i += info.len;
      }

      if (lineIdx == point.line && point.offset >= line.size() && !pointFound) {
        cur_row = row;
        cur_col = leftGutter + (vcol >= hScroll ? vcol - hScroll : 0);
        pointFound = true;
      }

      if (!overflows && vcol >= hScroll)
        emitLineEnd(lineIdx, line.size(), vcol - hScroll, cw, rs);

      emitEraseLineWithBg();

      // Gutter: show ↠ if content extends past the visible area.
      emitGutter(row, row == cur_row && pointFound,
                 overflows ? "\xe2\x86\xa0" : " "); // ↠ or space

      ++row;
      if (row < rows.value) {
        emitLeftGutter(row, lineIdx + 2 <= _lines.size() ? lineIdx + 2 : 0);
        emitRowStart(row);
      }
    }

    if (!pointFound) {
      cur_row = row;
      cur_col = leftGutter;
    }

    fillTrailingRows(row);

    return {RowIdx{cur_row}, ColIdx{cur_col}};
  }

  // Wrap mode.
  std::size_t row = 0, col = 0;
  // The viewport may begin partway into a wrapped line — show the line
  // number on the very first emitted row only when we're at the top of
  // the physical line; otherwise emit blanks so the number isn't
  // misleadingly attached to a continuation row.
  emitLeftGutter(0, viewFromOffset == 0 ? viewFrom.line.value + 1 : 0);
  emitRowStart(0);

  // Close the current visual row with the wrap indicator and start a
  // continuation row of the same physical line. Returns false if the
  // panel has no row left for it.
  auto wrapToNextRow = [&](RenderState& rs) {
    if (rs.bracketMatchActive) {
      out += ansi::DefaultFg;
      rs.bracketMatchActive = false;
    }
    if (rs.lastSynTok != SyntaxToken::Normal) {
      out += kSynReset;
      rs.lastSynTok = SyntaxToken::Normal;
    }
    if (rs.highlightActive) {
      out += ansi::Reset;
      rs.highlightActive = false;
    }
    if (rs.regionActive) {
      out += ansi::Reset;
      rs.regionActive = false;
    }
    emitEraseLineWithBg();
    emitGutter(row, row == cur_row && pointFound, "\xe2\x86\xb5"); // ↵
    ++row;
    col = 0;
    if (row >= rows.value)
      return false;
    // Continuation row: blank gutter so the line number stays
    // anchored to the row that begins the physical line.
    emitLeftGutter(row, 0);
    emitRowStart(row);
    if (curLineBg)
      out += curLineBg;
    return true;
  };

  for (std::size_t lineIdx = viewFrom.line.value; lineIdx < _lines.size() && row < rows.value;
       ++lineIdx) {
    auto const& line = *_lines[lineIdx];
    // For the viewport line, start from viewFromOffset (sub-line scroll).
    std::size_t startByte = (lineIdx == viewFrom.line.value) ? viewFromOffset : std::size_t{0};

    // Track whether the previous row was full (deferred wrap indicator).
    bool pendingWrap = false;
    RenderState rs;

    setupLine(lineIdx, line);

    for (std::size_t i = startByte; i < line.size() && row < rows.value;) {
      auto info = charInfoAt(line.data() + i, line.size() - i);

      // Wrap if the representation doesn't fit on the current line.
      if ((col > 0 && col + info.width > cw) || pendingWrap) {
        pendingWrap = false;
        if (!wrapToNextRow(rs))
          break;
      }

      if (lineIdx == point.line && i <= point.offset && point.offset < i + info.len &&
          !pointFound) {
        cur_row = row;
        cur_col = leftGutter + col;
        pointFound = true;
      }

      updateStyle(lineIdx, i, rs);
      emitChar(line.data() + i, info, rs);
      col += info.width;

      if (col >= cw)
        pendingWrap = true;

      i += info.len;
    }

    // A line whose text fills its last row exactly ends on a continuation
    // row of its own: the end-of-line position, and a cursor on it, need a
    // cell. Every row computation counts that row (computeRowBounds(),
    // screenRow(), ensureVisible()), so it is drawn whether or not the
    // cursor is there; drawing the next line on it instead put the cursor
    // on that line's first character.
    if (pendingWrap && row < rows.value)
      wrapToNextRow(rs);

    // Point at end of line (offset == line.size()).
    if (lineIdx == point.line && point.offset >= line.size() && !pointFound) {
      cur_row = row;
      cur_col = leftGutter + col;
      pointFound = true;
    }

    // End of physical line — emit newline visual and gutter (no wrap
    // indicator).
    if (row < rows.value) {
      emitLineEnd(lineIdx, line.size(), col, cw, rs);
      emitEraseLineWithBg();
      emitGutter(row, row == cur_row && pointFound,
                 " "); // natural line end, no indicator
      if (lineIdx + 1 < _lines.size()) {
        ++row;
        col = 0;
        if (row < rows.value) {
          emitLeftGutter(row, lineIdx + 2);
          emitRowStart(row);
        }
      }
    }
  }

  // If point is past all rendered content, use where the scan ended.
  if (!pointFound) {
    cur_row = row;
    cur_col = leftGutter + col;
  }

  fillTrailingRows(row);

  return {RowIdx{cur_row}, ColIdx{cur_col}};
}
