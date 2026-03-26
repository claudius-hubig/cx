#include "buffer.hpp"
#include "byte_constants.hpp"
#include "modes/c/syntax.hpp"
#include "modes/md/syntax.hpp"
#include "subprocess.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <span>

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

std::string Buffer::format(std::optional<std::pair<std::size_t, std::size_t>> lineRange) {
  if (readonly())
    return {};
  if (!hasPath())
    return "Save file first to determine format";
  if (_mode != BufferMode::Cpp)
    return "No formatter for this file type";

  BufferPoint endPt{_lines.size() - 1, _lines.back().size()};
  auto allText = extractRegion({0, 0}, endPt);

  std::vector<std::string> args = {"clang-format", "--assume-filename=" + _name};
  if (lineRange) {
    auto [startLine, endLine] = *lineRange;
    args.push_back("--lines=" + std::to_string(startLine) + ":" + std::to_string(endLine));
  }

  auto result = runSubprocess(args, allText);
  if (!result)
    return result.error();
  if (result->exitCode != 0) {
    auto& err = result->stderrData;
    auto nl = err.find('\n');
    return nl != std::string::npos ? err.substr(0, nl) : err;
  }
  if (result->stdoutData == allText)
    return "Already formatted";

  auto savedLine = _point.line;
  auto savedOffset = _point.offset;
  clearMark();
  deleteRegion({0, 0}, endPt);
  insertText(result->stdoutData);
  setPoint(savedLine, savedOffset);
  return "Formatted";
}

std::string Buffer::receiveTab() {
  if (readonly())
    return {};
  if (_mode == BufferMode::Cpp) {
    auto line = _point.line + 1; // 1-based
    auto msg = format(std::make_pair(line, line));
    // If cursor is before the first non-space character, move it there.
    auto& content = _lines[_point.line];
    std::size_t firstNonSpace = 0;
    while (firstNonSpace < content.size() && content[firstNonSpace] == ' ')
      ++firstNonSpace;
    if (_point.offset < firstNonSpace)
      setPoint(_point.line, firstNonSpace);
    return msg;
  }
  insert('\t');
  return {};
}

void Buffer::doInsert(uint32_t cp) {
  _rowBoundsCache.lineIdx = SIZE_MAX;
  invalidateSynCache(_point.line);
  auto [n, enc] = encodeUtf8(cp);

  auto insertLine = _point.line;
  auto insertOff = _point.offset;

  auto& line = _lines[_point.line];
  std::size_t autoIndent = 0;
  if (cp == '\n') {
    // Split current line at point.offset.
    std::string tail(line, _point.offset);
    line.erase(_point.offset);
    _lines.insert(_lines.begin() + _point.line + 1, std::move(tail));
    _point.line++;
    _point.offset = 0;
    // In Cpp mode, copy leading whitespace from the previous line for
    // auto-indent. This runs inside doInsert (after readonly() was already
    // checked by the public insert() wrapper).
    if (_mode == BufferMode::Cpp) {
      auto& prev = _lines[_point.line - 1];
      while (autoIndent < prev.size() && prev[autoIndent] == ' ')
        ++autoIndent;
      if (autoIndent > 0) {
        auto& cur = _lines[_point.line];
        cur.insert(cur.begin(), autoIndent, ' ');
        _point.offset = autoIndent;
      }
    }
  } else {
    auto* p = reinterpret_cast<char const*>(enc.data());
    line.insert(_point.offset, p, n);
    _point.offset += n;
  }
  _modified = true;

  // Adjust mark to track its original position.
  if (_mark) {
    auto& m = *_mark;
    if (cp == '\n') {
      if (m.line == insertLine && m.offset >= insertOff) {
        m.line = insertLine + 1;
        m.offset = m.offset - insertOff + autoIndent;
      } else if (m.line > insertLine) {
        m.line++;
      }
    } else {
      if (m.line == insertLine && m.offset >= insertOff)
        m.offset += n;
    }
  }

  if (_desiredCol) {
    std::size_t vcol = pointVisualCol();
    if (vcol >= _desiredCol->value)
      _desiredCol.reset();
  }
}

void Buffer::doDeleteAtPoint() {
  _rowBoundsCache.lineIdx = SIZE_MAX;
  invalidateSynCache(_point.line);
  auto& line = _lines[_point.line];
  if (_point.offset < line.size()) {
    // Delete within line.
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    auto end = std::min(_point.offset + len, line.size());
    auto delBytes = end - _point.offset;
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
    // Adjust mark.
    if (_mark && _mark->line == _point.line && _mark->offset > _point.offset) {
      if (_mark->offset >= _point.offset + delBytes)
        _mark->offset -= delBytes;
      else
        _mark->offset = _point.offset;
    }
  } else if (_point.line + 1 < _lines.size()) {
    // At end of line — merge next line into this one (delete the newline).
    auto lineLen = line.size();
    auto& next = _lines[_point.line + 1];
    line.insert(line.end(), next.begin(), next.end());
    _lines.erase(_lines.begin() + _point.line + 1);
    _modified = true;
    // Adjust mark.
    if (_mark) {
      if (_mark->line == _point.line + 1) {
        _mark->line = _point.line;
        _mark->offset += lineLen;
      } else if (_mark->line > _point.line + 1) {
        _mark->line--;
      }
    }
  }
}

void Buffer::doDeleteBeforePoint() {
  _rowBoundsCache.lineIdx = SIZE_MAX;
  // Invalidate from the line that will be affected: if at the start of
  // a line, the preceding line is joined and needs re-tokenisation.
  invalidateSynCache(_point.offset > 0 ? _point.line : (_point.line > 0 ? _point.line - 1 : 0));
  auto& line = _lines[_point.line];
  if (_point.offset > 0) {
    // Walk back over continuation bytes.
    auto end = _point.offset;
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
    auto delBytes = end - _point.offset;
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
    // Adjust mark.
    if (_mark && _mark->line == _point.line && _mark->offset > _point.offset) {
      if (_mark->offset >= _point.offset + delBytes)
        _mark->offset -= delBytes;
      else
        _mark->offset = _point.offset;
    }
  } else if (_point.line > 0) {
    // At start of line — merge into end of previous line (delete the newline).
    auto curLine = _point.line;
    auto& prev = _lines[_point.line - 1];
    std::size_t newOffset = prev.size();
    prev.insert(prev.end(), line.begin(), line.end());
    _lines.erase(_lines.begin() + _point.line);
    _point.line--;
    _point.offset = newOffset;
    _modified = true;
    // Adjust mark.
    if (_mark) {
      if (_mark->line == curLine) {
        _mark->line = _point.line;
        _mark->offset += newOffset;
      } else if (_mark->line > curLine) {
        _mark->line--;
      }
    }
  }
}

void Buffer::doDeleteChunkAtPoint() {
  if (atEnd())
    return;

  // Classify the character at point: blank (space/tab) or newline (at end of
  // line), vs non-blank.
  auto curIsBlank = [&]() -> bool {
    auto& line = _lines[_point.line];
    if (_point.offset < line.size())
      return isWhitespace(line[_point.offset]);
    // At end of line — the implicit newline counts as blank.
    return true;
  };

  bool startBlank = curIsBlank();
  do {
    doDeleteAtPoint();
  } while (!atEnd() && curIsBlank() == startBlank);
}

void Buffer::doDeleteChunkBeforePoint() {
  if (_point.line == 0 && _point.offset == 0)
    return;

  // Classify the character before point.
  auto prevIsBlank = [&]() -> bool {
    if (_point.offset > 0) {
      auto& line = _lines[_point.line];
      // Walk back over UTF-8 continuation bytes to find the lead byte.
      std::size_t pos = _point.offset - 1;
      while (pos > 0 && isUtf8Continuation(static_cast<unsigned char>(line[pos])))
        --pos;
      return isWhitespace(line[pos]);
    }
    // At start of line — the newline before this line counts as blank.
    return true;
  };

  bool startBlank = prevIsBlank();
  do {
    doDeleteBeforePoint();
  } while ((_point.line > 0 || _point.offset > 0) && prevIsBlank() == startBlank);
}

void Buffer::moveHome() {
  _point.offset = 0;
  _desiredCol.reset();
}

void Buffer::moveEnd() {
  _point.offset = _lines[_point.line].size();
  _desiredCol.reset();
}

void Buffer::moveLeft() {
  if (_point.offset > 0) {
    auto& line = _lines[_point.line];
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _lines[_point.line].size();
  }
  _desiredCol.reset();
}

void Buffer::moveRight() {
  auto& line = _lines[_point.line];
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  } else if (_point.line + 1 < _lines.size()) {
    ++_point.line;
    _point.offset = 0;
  }
  _desiredCol.reset();
}

bool Buffer::currentLineWraps(ColIdx cols) const {
  std::size_t cw = contentWidth(cols);
  return visualCol(_lines[_point.line], 0, _lines[_point.line].size()) > cw;
}

// Compute visual column width of bytes in line[from..to).
std::size_t Buffer::visualCol(std::string const& line, std::size_t from, std::size_t to) const {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

std::size_t Buffer::pointVisualCol() const {
  if (_pointColCache.line == _point.line && _pointColCache.offset == _point.offset)
    return _pointColCache.visualCol;
  // Compute both visual and codepoint columns in a single scan.
  _pointColCache.line = _point.line;
  _pointColCache.offset = _point.offset;
  _pointColCache.visualCol = 0;
  _pointColCache.codepointCol = 0;
  auto& line = _lines[_point.line];
  for (std::size_t i = 0; i < _point.offset && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    _pointColCache.visualCol += info.width;
    i += info.len;
    ++_pointColCache.codepointCol;
  }
  return _pointColCache.visualCol;
}

// Walk a line from `from` to find the byte offset matching a target visual
// column. Stops at `to` (or line.size() if to > line.size()).
static std::size_t offsetForVisualCol(std::string const& line, std::size_t from, std::size_t to,
                                      std::size_t goal) {
  std::size_t pos = from, v = 0;
  std::size_t end = std::min(to, line.size());
  while (pos < end) {
    auto info = charInfoAt(line.data() + pos, line.size() - pos);
    if (v + info.width > goal)
      break;
    v += info.width;
    pos += info.len;
  }
  return std::min(pos, end);
}

// Scan a line to find the visual-row boundaries around a byte offset.
// Returns {prevRowStart, curRowStart, nextRowStart} where:
//   curRowStart  = start of the visual row containing `byteOff`
//   prevRowStart = start of the visual row before curRowStart (or curRowStart
//                  if curRowStart == 0)
//   nextRowStart = start of the visual row after curRowStart (or line.size()
//                  if curRowStart is the last visual row)
struct VisualRowInfo {
  std::size_t prevRowStart;
  std::size_t curRowStart;
  std::size_t nextRowStart;
};

static VisualRowInfo findVisualRow(std::string const& line, std::size_t byteOff, std::size_t cols) {
  std::size_t prevStart = 0, curStart = 0;
  std::size_t col = 0;

  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
      // Wrap: new visual row starts at i.
      if (byteOff < i)
        return {prevStart, curStart, i};
      prevStart = curStart;
      curStart = i;
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      // Exact fit: new visual row starts at i + info.len.
      std::size_t next = i + info.len;
      if (byteOff < next)
        return {prevStart, curStart, next};
      prevStart = curStart;
      curStart = next;
      col = 0;
    }
    i += info.len;
  }
  // byteOff is on the last (or only) visual row.
  return {prevStart, curStart, line.size()};
}

// Find the byte offset at the start of the last visual row of a line.
static std::size_t lastVisualRowStart(std::string const& line, std::size_t cols) {
  std::size_t rowStart = 0, col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
      rowStart = i;
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      rowStart = i + info.len;
      col = 0;
    }
    i += info.len;
  }
  return rowStart;
}

std::vector<std::size_t> const& Buffer::ensureRowBounds(std::size_t lineIdx,
                                                        std::size_t cols) const {
  if (_rowBoundsCache.lineIdx == lineIdx && _rowBoundsCache.cols == cols)
    return _rowBoundsCache.rowStarts;

  _rowBoundsCache.lineIdx = lineIdx;
  _rowBoundsCache.cols = cols;
  _rowBoundsCache.rowStarts.clear();
  _rowBoundsCache.rowStarts.push_back(0);

  auto& line = _lines[lineIdx];
  std::size_t col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
      _rowBoundsCache.rowStarts.push_back(i);
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      _rowBoundsCache.rowStarts.push_back(i + info.len);
      col = 0;
    }
    i += info.len;
  }
  return _rowBoundsCache.rowStarts;
}

// Look up VisualRowInfo from cached row boundaries using binary search.
static VisualRowInfo findVisualRowCached(std::vector<std::size_t> const& rowStarts,
                                         std::size_t byteOff, std::size_t lineSize) {
  // Find the last row whose start <= byteOff.
  // upper_bound gives first element > byteOff; back up one.
  auto it = std::upper_bound(rowStarts.begin(), rowStarts.end(), byteOff);
  std::size_t rowIdx = static_cast<std::size_t>(it - rowStarts.begin()) - 1;

  std::size_t curStart = rowStarts[rowIdx];
  std::size_t prevStart = rowIdx > 0 ? rowStarts[rowIdx - 1] : curStart;
  std::size_t nextStart = rowIdx + 1 < rowStarts.size() ? rowStarts[rowIdx + 1] : lineSize;

  return {prevStart, curStart, nextStart};
}

void Buffer::moveUp(ColIdx cols) {
  auto& line = _lines[_point.line];
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);
  auto& bounds = ensureRowBounds(_point.line, cw);
  auto vr = findVisualRowCached(bounds, _point.offset, line.size());

  // Screen column within the current visual row.
  std::size_t vcol = visualCol(line, vr.curRowStart, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  std::size_t targetRowStart, targetRowEnd;
  if (vr.curRowStart > 0) {
    // Move to previous visual row of the same physical line.
    targetRowStart = vr.prevRowStart;
    targetRowEnd = vr.curRowStart;
  } else if (_point.line > 0) {
    // Move to last visual row of previous physical line.
    --_point.line;
    auto& prevBounds = ensureRowBounds(_point.line, cw);
    targetRowStart = prevBounds.back();
    targetRowEnd = _lines[_point.line].size();
  } else {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  _point.offset = offsetForVisualCol(_lines[_point.line], targetRowStart, targetRowEnd, goal);

  std::size_t actual = visualCol(_lines[_point.line], targetRowStart, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void Buffer::moveDown(ColIdx cols) {
  auto& line = _lines[_point.line];
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);
  auto& bounds = ensureRowBounds(_point.line, cw);
  auto vr = findVisualRowCached(bounds, _point.offset, line.size());

  // Screen column within the current visual row.
  std::size_t vcol = visualCol(line, vr.curRowStart, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  std::size_t targetRowStart, targetRowEnd;
  if (vr.nextRowStart < line.size()) {
    // Move to next visual row of the same physical line.
    targetRowStart = vr.nextRowStart;
    auto nextVr = findVisualRowCached(bounds, vr.nextRowStart, line.size());
    targetRowEnd = nextVr.nextRowStart;
  } else if (_point.line + 1 < _lines.size()) {
    // Move to first visual row of next physical line.
    ++_point.line;
    auto& next = _lines[_point.line];
    targetRowStart = 0;
    if (next.empty()) {
      targetRowEnd = 0;
    } else {
      auto& nextBounds = ensureRowBounds(_point.line, cw);
      targetRowEnd = nextBounds.size() > 1 ? nextBounds[1] : next.size();
    }
  } else {
    _point.offset = line.size();
    _desiredCol.reset();
    return;
  }

  _point.offset = offsetForVisualCol(_lines[_point.line], targetRowStart, targetRowEnd, goal);

  std::size_t actual = visualCol(_lines[_point.line], targetRowStart, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void Buffer::movePhysicalUp() {
  std::size_t vcol = pointVisualCol();
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line == 0) {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  --_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], 0, _lines[_point.line].size(), goal);

  std::size_t actual = pointVisualCol();
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void Buffer::movePhysicalDown() {
  std::size_t vcol = pointVisualCol();
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line + 1 >= _lines.size()) {
    _point.offset = _lines[_point.line].size();
    _desiredCol.reset();
    return;
  }

  ++_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], 0, _lines[_point.line].size(), goal);

  std::size_t actual = pointVisualCol();
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

bool Buffer::isBlankLine(std::size_t idx) const {
  if (idx >= _lines.size())
    return true;
  for (char c : _lines[idx])
    if (!isWhitespace(c))
      return false;
  return true;
}

void Buffer::moveWordRight() {
  auto& line = _lines[_point.line];
  // At very end of buffer?
  if (_point.line == _lines.size() - 1 && _point.offset >= line.size()) {
    _desiredCol.reset();
    return;
  }

  auto isSep = isWhitespace;

  // Step forward one code point (or cross to next line).
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  }
  // If at end of line, cross to next.
  if (_point.offset >= _lines[_point.line].size() && _point.line + 1 < _lines.size()) {
    ++_point.line;
    _point.offset = 0;
  }

  // Skip separators and line boundaries forward.
  for (;;) {
    auto& cur = _lines[_point.line];
    while (_point.offset < cur.size() && isSep(cur[_point.offset]))
      ++_point.offset;
    if (_point.offset < cur.size())
      break; // found non-sep
    // At end of line — cross to next.
    if (_point.line + 1 < _lines.size()) {
      ++_point.line;
      _point.offset = 0;
    } else {
      break;
    }
  }

  // Advance to the last char of this word.
  {
    auto& cur = _lines[_point.line];
    while (_point.offset < cur.size()) {
      auto len = utf8SeqLen(static_cast<unsigned char>(cur[_point.offset]));
      auto next = std::min(_point.offset + len, cur.size());
      // If next position is end of line or a separator, stop here.
      if (next >= cur.size() || isSep(cur[next]))
        break;
      _point.offset = next;
    }
  }
  _desiredCol.reset();
}

void Buffer::moveWordLeft() {
  if (_point.line == 0 && _point.offset == 0) {
    _desiredCol.reset();
    return;
  }

  auto isSep = isWhitespace;

  // Step back one code point (or cross to previous line).
  if (_point.offset > 0) {
    auto& line = _lines[_point.line];
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _lines[_point.line].size();
    // We're now at the "newline" position. Step back once more if possible.
    if (_point.offset > 0) {
      auto& line = _lines[_point.line];
      --_point.offset;
      while (_point.offset > 0 &&
             isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
        --_point.offset;
    }
  }

  // Skip separators and line boundaries backward.
  for (;;) {
    auto& cur = _lines[_point.line];
    if (_point.offset < cur.size() && !isSep(cur[_point.offset]))
      break; // on a non-sep char
    // Skip seps backward within line.
    bool foundNonSep = false;
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
        --prev;
      if (!isSep(cur[prev])) {
        _point.offset = prev;
        foundNonSep = true;
        break;
      }
      _point.offset = prev;
    }
    if (foundNonSep)
      break;
    // Check position 0.
    if (!cur.empty() && !isSep(cur[0])) {
      _point.offset = 0;
      break;
    }
    // Cross to previous line.
    if (_point.line > 0) {
      --_point.line;
      auto& prev = _lines[_point.line];
      _point.offset = prev.size();
      if (_point.offset > 0) {
        --_point.offset;
        while (_point.offset > 0 &&
               isUtf8Continuation(static_cast<unsigned char>(prev[_point.offset])))
          --_point.offset;
      }
    } else {
      break;
    }
  }

  // If currently on a separator, we're done.
  {
    auto& cur = _lines[_point.line];
    if (_point.offset < cur.size() && isSep(cur[_point.offset])) {
      _desiredCol.reset();
      return;
    }
  }

  // Walk backward to the first char of this word.
  {
    auto& cur = _lines[_point.line];
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
        --prev;
      if (isSep(cur[prev]))
        break;
      _point.offset = prev;
    }
  }
  _desiredCol.reset();
}

void Buffer::moveParagraphUp() {
  if (_point.line == 0) {
    _point = {0, 0};
    _desiredCol.reset();
    return;
  }

  // Go to the line above.
  std::size_t pos = _point.line - 1;

  // Skip blank lines upward.
  while (pos > 0 && isBlankLine(pos))
    --pos;

  // If at top and still blank, go to start.
  if (isBlankLine(pos)) {
    _point = {0, 0};
    _desiredCol.reset();
    return;
  }

  // On a non-blank line. Find the start of this paragraph.
  while (pos > 0 && !isBlankLine(pos - 1))
    --pos;

  _point = {pos, 0};
  _desiredCol.reset();
}

void Buffer::moveParagraphDown() {
  bool onBlank = isBlankLine(_point.line);
  std::size_t pos = _point.line + 1;

  // If current line was non-blank, skip non-blank lines (rest of paragraph).
  if (!onBlank) {
    while (pos < _lines.size() && !isBlankLine(pos))
      ++pos;
  }

  // Skip blank lines.
  while (pos < _lines.size() && isBlankLine(pos))
    ++pos;

  if (pos >= _lines.size())
    pos = _lines.size() - 1;
  _point = {pos, 0};
  _desiredCol.reset();
}

void Buffer::pageUp(RowIdx screenRows, ColIdx cols, bool wrap) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  for (std::size_t i = 0; i < half; ++i) {
    if (wrap)
      moveUp(cols);
    else
      movePhysicalUp();
    scrollUp(wrap);
  }
}

void Buffer::pageDown(RowIdx screenRows, ColIdx cols, bool wrap) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  for (std::size_t i = 0; i < half; ++i) {
    if (wrap)
      moveDown(cols);
    else
      movePhysicalDown();
    scrollDown();
  }
  // Clamp viewport so at least half a screen of content is visible.
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFrom.line.value > limit) {
    _viewFrom.line = RowIdx{limit};
    _viewFrom.col = ByteOffset{0};
  }
}

std::size_t Buffer::viewFromLinesBeforeEnd(std::size_t lines) const {
  if (lines >= _lines.size())
    return 0;
  return _lines.size() - lines;
}

void Buffer::scrollViewNearEnd(RowIdx screenRows) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFrom.line.value < limit)
    scrollDown();
}

void Buffer::scrollDown() {
  if (_viewFrom.line.value + 1 < _lines.size()) {
    _viewFrom.line = RowIdx{_viewFrom.line.value + 1};
    _viewFrom.col = ByteOffset{0};
  }
}

bool Buffer::gotoMatchingBracket() {
  auto match = findMatchingBracket(_lines, _point);
  if (!match)
    return false;
  _point = *match;
  _desiredCol.reset();
  return true;
}

void Buffer::gotoLine(std::size_t line, RowIdx screenRows) {
  if (line == 0)
    line = 1;
  // Convert to 0-based, clamp to last line.
  std::size_t idx = line - 1;
  if (idx >= _lines.size())
    idx = _lines.size() - 1;

  _point = {idx, 0};

  // Center _viewFrom so the target line is in the middle of the screen.
  std::size_t half = screenRows.value / 2;
  _viewFrom.line = RowIdx{idx >= half ? idx - half : 0};
  _viewFrom.col = ByteOffset{0};
}

void Buffer::setPoint(std::size_t line, std::size_t offset) {
  _point.line = std::min(line, _lines.size() - 1);
  _point.offset = std::min(offset, _lines[_point.line].size());
  _desiredCol.reset();
}

void Buffer::scrollUp(bool wrap) {
  if (wrap && std::holds_alternative<ByteOffset>(_viewFrom.col) && _viewFrom.byteOffset() > 0) {
    _viewFrom.col = ByteOffset{0};
  } else if (_viewFrom.line.value > 0) {
    _viewFrom.line = RowIdx{_viewFrom.line.value - 1};
    _viewFrom.col = ByteOffset{0};
  }
}

std::size_t Buffer::screenRow(ColIdx cols, bool wrap) const {
  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

  if (!wrap)
    return _point.line >= _viewFrom.line.value ? _point.line - _viewFrom.line.value : 0;

  std::size_t row = 0;
  for (std::size_t lineIdx = _viewFrom.line.value;
       lineIdx <= _point.line && lineIdx < _lines.size(); ++lineIdx) {
    auto& line = _lines[lineIdx];
    std::size_t end = (lineIdx == _point.line) ? _point.offset : line.size();
    // For the viewport line, start from the sub-line wrap offset.
    std::size_t start =
        (lineIdx == _viewFrom.line.value && wrap) ? _viewFrom.byteOffset() : std::size_t{0};

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
    if (lineIdx < _point.line) {
      ++row;
    }
  }
  return row;
}

void Buffer::ensureVisible(RowIdx rows, ColIdx cols, bool wrap) {
  // Reset col offset when switching wrap mode, to avoid stale scroll state.
  if (_wrapMode != wrap) {
    _wrapMode = wrap;
    _viewFrom.resetCol(wrap);
  }
  // Ensure the variant holds the correct type for the current mode.
  if (wrap && !std::holds_alternative<ByteOffset>(_viewFrom.col))
    _viewFrom.col = ByteOffset{0};
  else if (!wrap && !std::holds_alternative<VisualCol>(_viewFrom.col))
    _viewFrom.col = VisualCol{0};

  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

  if (!wrap) {
    auto& hs = _viewFrom.hScroll();

    // Vertical: simple line-based scrolling.
    if (_point.line < _viewFrom.line.value)
      _viewFrom.line = RowIdx{_point.line};
    if (_point.line >= _viewFrom.line.value + rows.value)
      _viewFrom.line = RowIdx{_point.line - rows.value + 1};

    // Horizontal: keep cursor within visible columns.
    std::size_t vcol = pointVisualCol();
    if (vcol < hs)
      hs = vcol;
    if (vcol >= hs + cw)
      hs = vcol - cw + 1;
    return;
  }

  auto& vfo = _viewFrom.byteOffset();

  // Handle point above the viewport (including sub-line offset).
  if (_point < viewFrom()) {
    _viewFrom.line = RowIdx{_point.line};
    vfo = 0;
  }

  // Handle point far below the viewport (line-level jump).
  if (_point.line >= _viewFrom.line.value + rows.value) {
    std::size_t half = rows.value / 2;
    _viewFrom.line = RowIdx{_point.line >= half ? _point.line - half : 0};
    vfo = 0;
  }

  // Single forward scan from the viewport to the point, counting visual
  // rows. We maintain a ring buffer of the last `rows` viewport-start
  // candidates {line, byteOffset}. When the scan finishes, the oldest
  // entry in the ring is the viewport position that places the cursor on
  // the last visible screen row.
  //
  // This replaces the previous while-loop that called screenRow() on each
  // iteration, avoiding the O(N²) cost of re-scanning from the viewport
  // on every line advance.

  struct ViewPos {
    std::size_t line;
    std::size_t byteOff;
  };
  std::size_t ringSize = rows.value;
  std::vector<ViewPos> ring(ringSize, {_viewFrom.line.value, vfo});
  std::size_t ringPos = 0;
  std::size_t visRow = 0;

  auto pushRing = [&](std::size_t line, std::size_t off) {
    ringPos = (ringPos + 1) % ringSize;
    ring[ringPos] = {line, off};
  };

  for (std::size_t lineIdx = _viewFrom.line.value;
       lineIdx <= _point.line && lineIdx < _lines.size(); ++lineIdx) {
    auto& line = _lines[lineIdx];
    std::size_t end = (lineIdx == _point.line) ? _point.offset : line.size();
    std::size_t start = (lineIdx == _viewFrom.line.value) ? vfo : std::size_t{0};
    std::size_t col = 0;

    for (std::size_t i = start; i < end && i < line.size();) {
      auto info = charInfoAt(line.data() + i, line.size() - i);
      if (col + info.width > cw) {
        ++visRow;
        pushRing(lineIdx, i);
        col = 0;
      }
      col += info.width;
      if (col >= cw) {
        ++visRow;
        pushRing(lineIdx, i + info.len);
        col = 0;
      }
      i += info.len;
    }
    if (lineIdx < _point.line) {
      ++visRow;
      pushRing(lineIdx + 1, 0);
    }
  }

  if (visRow < rows.value)
    return; // point is already on screen

  std::size_t readIdx = (ringPos + 1) % ringSize;
  _viewFrom.line = RowIdx{ring[readIdx].line};
  vfo = ring[readIdx].byteOff;
}

BufferPoint Buffer::viewFrom() const {
  std::size_t off = std::holds_alternative<ByteOffset>(_viewFrom.col) ? _viewFrom.byteOffset() : 0;
  return {_viewFrom.line.value, off};
}

std::tuple<std::size_t, std::size_t> Buffer::lineCol() const {
  // Ensure the cache is populated (also computes codepointCol).
  pointVisualCol();
  return {_point.line + 1, _pointColCache.codepointCol + 1};
}

void Buffer::toggleMark() {
  if (_mark)
    _mark.reset();
  else
    _mark = _point;
}

std::string Buffer::extractRegion(BufferPoint from, BufferPoint to) const {
  if (to < from)
    std::swap(from, to);
  if (from.line >= _lines.size())
    return {};

  // Rough size estimate to avoid repeated reallocations.
  std::size_t estimate = 0;
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l) {
    auto& line = _lines[l];
    std::size_t start = (l == from.line) ? std::min(from.offset, line.size()) : 0;
    std::size_t end = (l == to.line) ? std::min(to.offset, line.size()) : line.size();
    estimate += end - start + 1; // +1 for newline
  }
  std::string result;
  result.reserve(estimate);
  for (std::size_t l = from.line; l <= to.line && l < _lines.size(); ++l) {
    auto& line = _lines[l];
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

void Buffer::deleteRegion(BufferPoint from, BufferPoint to) {
  _rowBoundsCache.lineIdx = SIZE_MAX;
  if (to < from)
    std::swap(from, to);
  invalidateSynCache(from.line);
  if (from.line >= _lines.size())
    return;

  to.line = std::min(to.line, _lines.size() - 1);
  to.offset = std::min(to.offset, _lines[to.line].size());
  from.offset = std::min(from.offset, _lines[from.line].size());

  // Splice: head of from-line + tail of to-line.
  auto& fromLine = _lines[from.line];
  auto& toLine = _lines[to.line];
  std::string merged(fromLine, 0, from.offset);
  merged.append(toLine, to.offset);

  // Erase lines [from.line .. to.line], replace with merged.
  _lines.erase(_lines.begin() + from.line, _lines.begin() + to.line + 1);
  _lines.insert(_lines.begin() + from.line, std::move(merged));

  _point = from;
  _mark.reset();
  _modified = true;
}

BufferPoint Buffer::insertText(std::string_view text) {
  _rowBoundsCache.lineIdx = SIZE_MAX;
  invalidateSynCache(_point.line);
  auto startLine = _point.line;
  auto startOff = _point.offset;

  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      // Split current line at _point.
      auto& line = _lines[_point.line];
      std::string tail(line, _point.offset);
      line.erase(_point.offset);
      _lines.insert(_lines.begin() + _point.line + 1, std::move(tail));
      _point.line++;
      _point.offset = 0;
      ++i;
    } else {
      // Batch-insert all bytes up to the next newline or end of text.
      auto end = text.find('\n', i);
      if (end == std::string_view::npos)
        end = text.size();
      auto& line = _lines[_point.line];
      line.insert(line.begin() + _point.offset, text.begin() + i, text.begin() + end);
      _point.offset += end - i;
      i = end;
    }
  }
  _modified = true;

  // Adjust mark to track its original position.
  if (_mark) {
    auto linesAdded = _point.line - startLine;
    if (_mark->line == startLine && _mark->offset >= startOff) {
      _mark->line += linesAdded;
      if (linesAdded > 0)
        _mark->offset = _mark->offset - startOff + _point.offset;
      else
        _mark->offset += _point.offset - startOff;
    } else if (_mark->line > startLine) {
      _mark->line += linesAdded;
    }
  }

  return _point;
}

void Buffer::invalidateSynCache(std::size_t fromLine) {
  if (fromLine < _synCacheValidUpto)
    _synCacheValidUpto = fromLine;
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
    if (_mode == BufferMode::Md)
      _synCache[i].tokens = tokenizeMarkdownLine(_lines[i], state);
    else
      _synCache[i].tokens = tokenizeLine(_lines[i], state);
    _synCache[i].stateAfter = state; // state was advanced by tokenizeLine
  }
  _synCacheValidUpto = upToLine;
}

std::tuple<RowIdx, ColIdx> Buffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                          bool showWhitespace, bool wrap) const {
  static constexpr auto kCtrlColor = "\x1b[38;2;204;120;50m";
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
  static constexpr auto kSynReset = "\x1b[22;39m";               // bold off + default fg
  // Bracket match: turquoise foreground (no bold/underline).
  static constexpr auto kBracketMatch = "\x1b[38;2;60;170;170m";

  auto emitRowStart = [&](std::size_t r) { out += ansi::cursorTo(r, 0); };

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
      } else if (*data == ' ' && showWhitespace) {
        out += kSpaceStyle;
        out += kWsFg;
        out += "\xc2\xb7";
        out += ansi::Reset;
        rs = {};
      } else if (u == 0) {
        out += kCtrlColor;
        out += "\\0";
        out += ansi::Reset;
        rs = {};
      } else if (u == 0x7f) {
        out += kCtrlColor;
        out += "^?";
        out += ansi::Reset;
        rs = {};
      } else if (u < 0x20) {
        out += kCtrlColor;
        out += '^';
        out += static_cast<char>('@' + u);
        out += ansi::Reset;
        rs = {};
      } else {
        out += *data;
      }
    } else if (info.len == 1) {
      // Invalid or lone continuation byte: display as hex.
      out += kCtrlColor;
      auto nibble = [](unsigned char n) -> char { return n < 10 ? ('0' + n) : ('a' + n - 10); };
      out += nibble(u >> 4);
      out += nibble(u & 0xf);
      out += ansi::Reset;
      rs = {};
    } else {
      out.append(data, info.len);
    }
  };

  // Content width excludes the gutter column.
  std::size_t cw = contentWidth(cols);

  // Emit the gutter character at column cols.value for a given row.
  // `highlight` marks the visual row containing the cursor.
  auto emitGutter = [&](std::size_t r, bool highlight, char const* indicator) {
    out += ansi::cursorTo(r, cols.value - 1);
    out += highlight ? ansi::Reset : ansi::ReverseVideo;
    out += indicator;
    out += ansi::Reset;
  };

  // Region highlighting: compute normalized start/end from mark and point.
  bool hasRegion = _mark.has_value();
  BufferPoint regionStart, regionEnd;
  if (hasRegion) {
    regionStart = std::min(_mark.value(), _point);
    regionEnd = std::max(_mark.value(), _point);
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
    if (!_highlight)
      return false;
    BufferPoint p{lineIdx, byteOffset};
    return _highlight->from <= p && p < _highlight->to;
  };

  std::size_t cur_row = 0, cur_col = 0;
  bool pointFound = false;

  // Extract viewport offsets from _viewFrom.
  std::size_t hScroll =
      (!wrap && std::holds_alternative<VisualCol>(_viewFrom.col)) ? _viewFrom.hScroll() : 0;
  std::size_t viewFromOffset =
      (wrap && std::holds_alternative<ByteOffset>(_viewFrom.col)) ? _viewFrom.byteOffset() : 0;

  // Syntax highlighting pre-computation.
  bool syntaxActive = _mode == BufferMode::Cpp || _mode == BufferMode::Md;
  std::optional<BufferPoint> matchBracket;
  std::span<SyntaxToken const> lineTokens;

  if (syntaxActive) {
    // Build the per-line cache up to just past the last potentially visible
    // line. This replaces the O(viewFrom) advanceBlockCommentState loop with
    // an incremental O(changed-lines) update.
    std::size_t lastVisible =
        std::min(_viewFrom.line.value + static_cast<std::size_t>(rows.value) + 1, _lines.size());
    ensureSynCache(lastVisible);
    matchBracket = findMatchingBracket(_lines, _point);
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
      return ansi::Bold;
    case SyntaxToken::Link:
      return kSynLink;
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
      }
      out += kHighlightOn;
      rs.highlightActive = true;
    } else if (!wantHl && rs.highlightActive) {
      out += ansi::Reset;
      rs.highlightActive = false;
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
                           (lineIdx == _point.line && byteOff == _point.offset));
      if (wantMatch && !rs.bracketMatchActive) {
        out += kBracketMatch;
        rs.bracketMatchActive = true;
      } else if (!wantMatch && rs.bracketMatchActive) {
        out += ansi::DefaultFg;
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
    }
    if (rs.regionActive) {
      out += ansi::Reset;
      rs.regionActive = false;
    }
    if (showWhitespace && lineIdx + 1 < _lines.size() && visCol < maxCol &&
        !inRegion(lineIdx, lineSize)) {
      out += kNlStyle;
      out += kWsFg;
      out += "\xe2\x86\xb5";
      out += ansi::Reset;
    }
  };

  if (!wrap) {
    // No-wrap: each physical line is one screen row, horizontally scrolled.
    std::size_t row = 0;
    emitRowStart(0);

    for (std::size_t lineIdx = _viewFrom.line.value; lineIdx < _lines.size() && row < rows.value;
         ++lineIdx) {
      auto& line = _lines[lineIdx];
      std::size_t vcol = 0;
      bool overflows = false; // content extends past visible area
      RenderState rs;

      if (syntaxActive)
        lineTokens = _synCache[lineIdx].tokens;

      for (std::size_t i = 0; i < line.size();) {
        auto info = charInfoAt(line.data() + i, line.size() - i);

        if (lineIdx == _point.line && i == _point.offset && !pointFound) {
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

      if (lineIdx == _point.line && _point.offset >= line.size() && !pointFound) {
        cur_row = row;
        cur_col = vcol >= hScroll ? vcol - hScroll : 0;
        pointFound = true;
      }

      if (!overflows && vcol >= hScroll)
        emitLineEnd(lineIdx, line.size(), vcol - hScroll, cw, rs);

      out += ansi::EraseLine;

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

    for (std::size_t r = row; r < rows.value; ++r) {
      if (r > row)
        emitRowStart(r);
      out += ansi::EraseLine;
      emitGutter(r, false, " ");
    }

    return {RowIdx{cur_row}, ColIdx{cur_col}};
  }

  // Wrap mode.
  std::size_t row = 0, col = 0;
  emitRowStart(0);

  for (std::size_t lineIdx = _viewFrom.line.value; lineIdx < _lines.size() && row < rows.value;
       ++lineIdx) {
    auto& line = _lines[lineIdx];
    // For the viewport line, start from viewFromOffset (sub-line scroll).
    std::size_t startByte = (lineIdx == _viewFrom.line.value) ? viewFromOffset : std::size_t{0};

    // Track whether the previous row was full (deferred wrap indicator).
    bool pendingWrap = false;
    RenderState rs;

    if (syntaxActive)
      lineTokens = _synCache[lineIdx].tokens;

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
        out += ansi::EraseLine;
        // We know there's more content — this is a true wrap.
        emitGutter(row, row == cur_row && pointFound, "\xe2\x86\xb5"); // ↵
        ++row;
        col = 0;
        pendingWrap = false;
        if (row >= rows.value)
          break;
        emitRowStart(row);
      }

      if (lineIdx == _point.line && i == _point.offset && !pointFound) {
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
    if (lineIdx == _point.line && _point.offset >= line.size() && !pointFound) {
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
      out += ansi::EraseLine;
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

  // Clear remaining rows.
  for (std::size_t r = row; r < rows.value; ++r) {
    if (r > row)
      emitRowStart(r);
    out += ansi::EraseLine;
    emitGutter(r, false, " ");
  }

  return {RowIdx{cur_row}, ColIdx{cur_col}};
}
