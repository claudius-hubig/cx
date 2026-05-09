#include "buffer.hpp"
#include "byte_constants.hpp"
#include "modes/c/syntax.hpp"
#include "modes/dif/syntax.hpp"
#include "modes/jsn/syntax.hpp"
#include "modes/md/syntax.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <span>

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

// --- Position-explicit editing methods ---

BufferPoint Buffer::insertAt(uint32_t cp, BufferPoint at) {
  invalidateSynCache(at.line);
  auto [n, enc] = encodeUtf8(cp);

  auto& line = mutLine(at.line);
  if (cp == '\n') {
    std::string tail(line, at.offset);
    line.resize(at.offset);

    std::size_t autoIndent = 0;
    if (_mode == BufferMode::Cpp || _mode == BufferMode::Jsn) {
      while (autoIndent < line.size() && line[autoIndent] == ' ')
        ++autoIndent;
    }

    if (autoIndent > 0) {
      std::string indented;
      indented.reserve(autoIndent + tail.size());
      indented.append(autoIndent, ' ');
      indented.append(tail);
      _lines.insert(_lines.begin() + at.line + 1, makeLine(std::move(indented)));
    } else {
      _lines.insert(_lines.begin() + at.line + 1, makeLine(std::move(tail)));
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
    invalidateSynCache(at.line);
    auto& line = mutLine(at.line);
    auto len = utf8SeqLen(static_cast<unsigned char>(line[at.offset]));
    auto end = std::min(at.offset + len, line.size());
    line.erase(line.begin() + at.offset, line.begin() + end);
    _modified = true;
    ++_docVersion;
  } else if (at.line + 1 < _lines.size()) {
    invalidateSynCache(at.line);
    // Take the next line's content out of the vector first so the
    // mutLine call below operates on a stable index.
    std::string nextContent = lineContent(at.line + 1);
    _lines.erase(_lines.begin() + at.line + 1);
    auto& line = mutLine(at.line);
    line.append(nextContent);
    _modified = true;
    ++_docVersion;
  }
  return at; // cursor doesn't move for delete-at
}

BufferPoint Buffer::deleteBefore(BufferPoint at) {
  if (at.offset > 0) {
    invalidateSynCache(at.line);
    auto& line = mutLine(at.line);
    auto end = at.offset;
    --at.offset;
    while (at.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[at.offset])))
      --at.offset;
    line.erase(line.begin() + at.offset, line.begin() + end);
    _modified = true;
    ++_docVersion;
  } else if (at.line > 0) {
    invalidateSynCache(at.line - 1);
    std::string lineContentCopy = lineContent(at.line);
    _lines.erase(_lines.begin() + at.line);
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
  invalidateSynCache(at.line);
  if (text.empty())
    return at;

  std::size_t newlineCount = 0;
  for (char c : text)
    if (c == '\n')
      ++newlineCount;

  if (newlineCount == 0) {
    auto& line = mutLine(at.line);
    line.insert(at.offset, text.data(), text.size());
    at.offset += text.size();
    _modified = true;
    ++_docVersion;
    return at;
  }

  // Save tail past insertion point, truncate the line there.
  std::string tail(lineContent(at.line), at.offset);
  mutLine(at.line).resize(at.offset);

  // Open a gap for new lines (single vector shift instead of K shifts).
  // Each gap LinePtr will be replaced with a freshly allocated string
  // below; share a single empty placeholder until then.
  _lines.insert(_lines.begin() + at.line + 1, newlineCount, makeLine());

  // Fill lines from text chunks separated by '\n'.
  std::size_t lineIdx = at.line;
  std::size_t i = 0;
  while (i < text.size()) {
    auto nl = text.find('\n', i);
    std::size_t chunkEnd = (nl == std::string_view::npos) ? text.size() : nl;

    std::string_view chunk(text.data() + i, chunkEnd - i);
    if (lineIdx == at.line)
      mutLine(lineIdx).append(chunk);
    else
      _lines[lineIdx] = makeLine(chunk);

    if (nl != std::string_view::npos) {
      ++lineIdx;
      i = nl + 1;
    } else {
      at.offset = chunkEnd - i;
      i = chunkEnd;
    }
  }

  if (text.back() == '\n') {
    at.line = lineIdx;
    at.offset = 0;
  } else {
    at.line = lineIdx;
  }

  mutLine(at.line).append(tail);
  _modified = true;
  ++_docVersion;
  return at;
}

void Buffer::deleteRegion(BufferPoint from, BufferPoint to) {
  if (to < from)
    std::swap(from, to);
  invalidateSynCache(from.line);
  if (from.line >= _lines.size())
    return;

  to.line = std::min(to.line, _lines.size() - 1);
  to.offset = std::min(to.offset, lineContent(to.line).size());
  from.offset = std::min(from.offset, lineContent(from.line).size());

  if (from.line == to.line) {
    mutLine(from.line).erase(from.offset, to.offset - from.offset);
  } else {
    std::string toTail(lineContent(to.line), to.offset);
    auto& fromLine = mutLine(from.line);
    fromLine.resize(from.offset);
    fromLine.append(toTail);
    _lines.erase(_lines.begin() + from.line + 1, _lines.begin() + to.line + 1);
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
      if (col + info.width > cw) {
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
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l) {
    auto const& line = *_lines[l];
    std::size_t start = (l == from.line) ? std::min(from.offset, line.size()) : 0;
    std::size_t end = (l == to.line) ? std::min(to.offset, line.size()) : line.size();
    // +1 for the inter-line '\n'. The emission loop only writes one
    // between adjacent lines, never after the last, so omit the +1
    // for the final line and avoid a 1-byte over-reservation.
    estimate += end - start + (l < to.line ? 1 : 0);
  }
  std::string result;
  result.reserve(estimate);
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l) {
    auto const& line = *_lines[l];
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

void Buffer::invalidateSynCache(std::size_t fromLine) {
  if (fromLine < _synCacheValidUpto)
    _synCacheValidUpto = fromLine;
}

std::span<SyntaxToken const> Buffer::cachedLineTokens(std::size_t lineIdx) const {
  ensureSynCache(lineIdx + 1);
  return _synCache[lineIdx].tokens;
}

void Buffer::ensureSynCache(std::size_t upToLine) const {
  // Resize if _lines changed (insertions / deletions).
  if (_synCache.size() != _lines.size()) {
    _synCache.resize(_lines.size());
    if (_synCacheValidUpto > _lines.size())
      _synCacheValidUpto = 0;
  }
  if (upToLine > _lines.size())
    upToLine = _lines.size();
  if (_synCacheValidUpto >= upToLine)
    return; // already covered

  // Start from the state at the end of the previous valid line.
  SyntaxState state;
  if (_synCacheValidUpto > 0)
    state = _synCache[_synCacheValidUpto - 1].stateAfter;

  for (std::size_t i = _synCacheValidUpto; i < upToLine; ++i) {
    auto const& line = *_lines[i];
    if (_mode == BufferMode::Md)
      tokenizeMarkdownLine(line, state, _synCache[i].tokens);
    else if (_mode == BufferMode::Jsn)
      tokenizeJsonLine(line, state, _synCache[i].tokens);
    else if (_mode == BufferMode::Dif)
      tokenizeDiffLine(line, state, _synCache[i].tokens);
    else if (_mode == BufferMode::Cpp)
      tokenizeLine(line, state, _synCache[i].tokens);
    else
      _synCache[i].tokens.assign(line.size(), SyntaxToken::Normal);
    _synCache[i].stateAfter = state;

    if (_hasConflicts && isConflictMarker(line))
      _synCache[i].tokens.assign(line.size(), SyntaxToken::ConflictMarker);
  }
  _synCacheValidUpto = upToLine;
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

  auto emitRowStart = [&](std::size_t r) { ansi::appendCursorTo(out, rowOffset + r, colOffset); };

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
        out += *data;
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
    } else {
      out.append(data, info.len);
    }
  };

  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

  // Emit the gutter character at column cols.value for a given row.
  // `highlight` marks the visual row containing the cursor.
  auto emitGutter = [&](std::size_t r, bool isCursorRow, char const* indicator) {
    ansi::appendCursorTo(out, rowOffset + r, colOffset + cols.value - 1);
    out += (isCursorRow && focused) ? ansi::Reset : ansi::ReverseVideo;
    out += indicator;
    out += ansi::Reset;
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
    bool wantRegion = !wantHl && inRegion(lineIdx, byteOff);
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
      lineTokens = _synCache[lineIdx].tokens;
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
  auto fillTrailingRows = [&](std::size_t startRow) {
    for (std::size_t r = startRow; r < rows.value; ++r) {
      if (r > startRow)
        emitRowStart(r);
      out += ansi::EraseLine;
      emitGutter(r, false, " ");
    }
  };

  if (!wrap) {
    // No-wrap: each physical line is one screen row, horizontally scrolled.
    std::size_t row = 0;
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

        if (lineIdx == point.line && i == point.offset && !pointFound) {
          cur_row = row;
          cur_col = vcol >= hScroll ? vcol - hScroll : 0;
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
        cur_col = vcol >= hScroll ? vcol - hScroll : 0;
        pointFound = true;
      }

      if (!overflows && vcol >= hScroll)
        emitLineEnd(lineIdx, line.size(), vcol - hScroll, cw, rs);

      emitEraseLineWithBg();

      // Gutter: show ↠ if content extends past the visible area.
      emitGutter(row, row == cur_row && pointFound,
                 overflows ? "\xe2\x86\xa0" : " "); // ↠ or space

      ++row;
      if (row < rows.value)
        emitRowStart(row);
    }

    if (!pointFound) {
      cur_row = row;
      cur_col = 0;
    }

    fillTrailingRows(row);

    return {RowIdx{cur_row}, ColIdx{cur_col}};
  }

  // Wrap mode.
  std::size_t row = 0, col = 0;
  emitRowStart(0);

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
      if (col + info.width > cw || pendingWrap) {
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
        // We know there's more content — this is a true wrap.
        emitGutter(row, row == cur_row && pointFound, "\xe2\x86\xb5"); // ↵
        ++row;
        col = 0;
        pendingWrap = false;
        if (row >= rows.value)
          break;
        emitRowStart(row);
        if (curLineBg)
          out += curLineBg;
      }

      if (lineIdx == point.line && i == point.offset && !pointFound) {
        cur_row = row;
        cur_col = col;
        pointFound = true;
      }

      updateStyle(lineIdx, i, rs);
      emitChar(line.data() + i, info, rs);
      col += info.width;

      if (col >= cw)
        pendingWrap = true;

      i += info.len;
    }

    // Point at end of line (offset == line.size()).
    // Must check before clearing pendingWrap: if the line ended exactly
    // at cw, the cursor belongs on the next visual row.
    if (lineIdx == point.line && point.offset >= line.size() && !pointFound) {
      if (pendingWrap) {
        cur_row = row + 1;
        cur_col = 0;
      } else {
        cur_row = row;
        cur_col = col;
      }
      pointFound = true;
    }

    // If the line ended exactly at cw, pendingWrap is true but there's
    // no more content to wrap — clear it so the outer loop emits blank.
    pendingWrap = false;

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
        if (row < rows.value)
          emitRowStart(row);
      }
    }
  }

  // If point is past all rendered content, use where the scan ended.
  if (!pointFound) {
    cur_row = row;
    cur_col = col;
  }

  fillTrailingRows(row);

  return {RowIdx{cur_row}, ColIdx{cur_col}};
}
