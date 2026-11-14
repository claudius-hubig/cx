#include "grep_buffer.hpp"
#include "byte_constants.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <utility>

namespace {

/**
 * @brief Truncate a (possibly ANSI-styled) display string to at most
 *        @p maxCols visual columns.
 *
 * Cuts only on whole character / escape-sequence boundaries so a
 * multi-byte char or CSI sequence is never split. CSI-aware because
 * display lines carry escapeNonPrintable() styling for control bytes
 * that appear in matched text.
 */
std::string truncateDisplayToWidth(std::string_view s, std::size_t maxCols) {
  std::size_t col = 0, i = 0;
  while (i < s.size()) {
    auto info = displayCharInfoAt(s.data() + i, s.size() - i);
    if (col + info.width > maxCols)
      break;
    col += info.width;
    i += info.len;
  }
  return std::string(s.substr(0, i));
}

/** @brief Drop leading spaces and tabs so deeply indented hits stay readable. */
std::string_view trimLeadingWhitespace(std::string_view s) {
  std::size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t'))
    ++i;
  return s.substr(i);
}

} // namespace

std::vector<GrepHit> parseGrepOutput(std::string_view out, std::filesystem::path const& root) {
  std::vector<GrepHit> hits;
  std::shared_ptr<GrepFile const> file;
  std::size_t pos = 0;
  while (pos < out.size()) {
    // The path runs up to the NUL, so a filename containing '\n' stays
    // intact; only the text after the line number ends at '\n'.
    auto nul = out.find('\0', pos);
    if (nul == std::string_view::npos)
      break;
    auto eol = out.find('\n', nul);
    auto record =
        out.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
    pos = (eol == std::string_view::npos) ? out.size() : eol + 1;
    nul -= record.data() - out.data();
    auto pathPart = record.substr(0, nul);
    if (pathPart.empty())
      continue;

    // Digits up to the separator, which is NUL for git grep and ':' for
    // grep. Anything else means this is not a result record.
    std::size_t i = nul + 1;
    std::size_t line = 0;
    std::size_t digits = 0;
    while (i < record.size() && record[i] >= '0' && record[i] <= '9') {
      auto d = static_cast<std::size_t>(record[i] - '0');
      // A line number this large cannot address a real file; treat the
      // record as malformed rather than wrapping around.
      if (line > (static_cast<std::size_t>(-1) - d) / 10) {
        digits = 0;
        break;
      }
      line = line * 10 + d;
      ++digits;
      ++i;
    }
    if (digits == 0 || line == 0 || i >= record.size())
      continue;
    if (record[i] != '\0' && record[i] != ':')
      continue;
    ++i;

    if (!file || file->displayPath != pathPart) {
      std::filesystem::path p(pathPart);
      file =
          std::make_shared<GrepFile const>(p.is_absolute() ? p : root / p, std::string(pathPart));
    }
    hits.push_back({file, line, std::string(trimLeadingWhitespace(record.substr(i)))});
  }
  return hits;
}

GrepBuffer::GrepBuffer() : Buffer("*grep*") { rebuild(); }

ViewHint GrepBuffer::refresh(std::string pattern, std::filesystem::path root,
                             std::vector<GrepHit> hits, bool truncated) {
  _pattern = std::move(pattern);
  _root = std::move(root);
  _hits = std::move(hits);
  _truncated = truncated;
  return rebuild();
}

ViewHint GrepBuffer::rebuild() {
  // Built in a vector and moved into the store (see LineStore).
  std::vector<LinePtr> lines;
  lines.reserve(kHeaderLines + _hits.size());
  _prefixLens.clear();
  _prefixLens.reserve(kHeaderLines + _hits.size());

  // Header: what was searched for, where, and how many hits came back.
  // Escaped because the pattern is arbitrary user input and the root can
  // contain control bytes from a hostile directory name.
  std::string header;
  if (_hits.empty())
    header = "No matches for \"" + escapeNonPrintable(_pattern) + "\"";
  else
    header = std::to_string(_hits.size()) + (_hits.size() == 1 ? " match" : " matches") +
             " for \"" + escapeNonPrintable(_pattern) + "\"";
  header += " in " + escapeNonPrintable(_root.string());
  if (_truncated)
    header += " (search stopped at the limit)";
  lines.push_back(makeLine(std::move(header)));
  _prefixLens.push_back(0);

  for (auto const& h : _hits) {
    auto line = escapeNonPrintable(h.displayPath()) + ":" + std::to_string(h.line) + ":";
    _prefixLens.push_back(line.size());
    line += ' ';
    line += escapeNonPrintable(h.text);
    lines.push_back(makeLine(std::move(line)));
  }
  _lines = LineStore(std::move(lines));

  // Land the cursor on the first result, not the header.
  return ViewHint{{_hits.empty() ? 0 : kHeaderLines, 0}, {}};
}

std::optional<GrepHit> GrepBuffer::selectedHit(BufferPoint p) const {
  if (p.line < kHeaderLines)
    return std::nullopt;
  auto idx = p.line - kHeaderLines;
  if (idx >= _hits.size())
    return std::nullopt;
  return _hits[idx];
}

std::tuple<RowIdx, ColIdx> GrepBuffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                              RenderParams const& params) const {
  std::size_t curLine = params.point.line;
  std::size_t scrollLine = params.viewFrom.line.value;
  std::size_t rowOffset = params.rowOffset;
  std::size_t colOffset = params.colOffset;

  for (std::size_t r = 0; r < rows.value; ++r) {
    std::size_t lineIdx = scrollLine + r;
    ansi::appendCursorTo(out, rowOffset + r, colOffset);

    if (lineIdx < _lines.size()) {
      bool highlight = (lineIdx == curLine);
      if (highlight)
        out += ansi::ReverseVideo;

      // Clamp to the panel width, reserving the final column for the
      // gutter indicator, so a long path or match can't bleed past the
      // panel into a sibling pane.
      std::string_view full = *_lines[lineIdx];
      std::size_t budget = cols.value > 1 ? cols.value - 1 : 0;
      std::string clipped = truncateDisplayToWidth(full, budget);

      // Dim the "path:line:" prefix so the matched text reads first. The
      // prefix length is a character boundary in the untruncated string,
      // so a clamp either keeps all of it or lands inside it.
      std::size_t prefixLen = std::min(_prefixLens[lineIdx], clipped.size());
      if (prefixLen > 0 && !highlight) {
        out += ansi::Dim;
        out += clipped.substr(0, prefixLen);
        out += ansi::DimOff;
        out += clipped.substr(prefixLen);
      } else {
        out += clipped;
      }
      // A clamp may have cut between an escaped byte's colour-on and
      // colour-off; reset so styling never leaks into the cleared
      // remainder or the gutter cell.
      if (clipped.size() < full.size())
        out += ansi::Reset;

      if (highlight)
        out += ansi::Reset;
      out += ansi::EraseLine;
    } else {
      out += ansi::EraseLine;
    }

    // Gutter column.
    ansi::appendCursorTo(out, rowOffset + r, colOffset + cols.value - 1);
    out += ansi::ReverseVideo;
    out += ' ';
    out += ansi::Reset;
  }

  std::size_t curScreenRow = curLine >= scrollLine ? curLine - scrollLine : 0;
  return {RowIdx{curScreenRow}, ColIdx{0}};
}
