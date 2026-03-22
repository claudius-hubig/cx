#include "buffer.hpp"
#include "char_width.hpp"

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

void Buffer::doInsert(uint32_t cp) {
  _mark.reset();
  char buf[4];
  std::size_t n;
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    buf[0] = static_cast<char>(0xE0 | (cp >> 12));
    buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    buf[0] = static_cast<char>(0xF0 | (cp >> 18));
    buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }

  auto& line = _lines[_point.line];
  if (cp == '\n') {
    // Split current line at point.offset.
    std::vector<char> tail(line.begin() + _point.offset, line.end());
    line.erase(line.begin() + _point.offset, line.end());
    _lines.insert(_lines.begin() + _point.line + 1, std::move(tail));
    _point.line++;
    _point.offset = 0;
  } else {
    line.insert(line.begin() + _point.offset, buf, buf + n);
    _point.offset += n;
  }
  _modified = true;

  if (_desiredCol) {
    std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
    if (vcol >= _desiredCol->value)
      _desiredCol.reset();
  }
}

void Buffer::doDeleteAtPoint() {
  _mark.reset();
  auto& line = _lines[_point.line];
  if (_point.offset < line.size()) {
    // Delete within line.
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    auto end = std::min(_point.offset + len, line.size());
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
  } else if (_point.line + 1 < _lines.size()) {
    // At end of line — merge next line into this one (delete the newline).
    auto& next = _lines[_point.line + 1];
    line.insert(line.end(), next.begin(), next.end());
    _lines.erase(_lines.begin() + _point.line + 1);
    _modified = true;
  }
}

void Buffer::doDeleteBeforePoint() {
  _mark.reset();
  auto& line = _lines[_point.line];
  if (_point.offset > 0) {
    // Walk back over continuation bytes.
    auto end = _point.offset;
    --_point.offset;
    while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
  } else if (_point.line > 0) {
    // At start of line — merge into end of previous line (delete the newline).
    auto& prev = _lines[_point.line - 1];
    std::size_t newOffset = prev.size();
    prev.insert(prev.end(), line.begin(), line.end());
    _lines.erase(_lines.begin() + _point.line);
    _point.line--;
    _point.offset = newOffset;
    _modified = true;
  }
}

void Buffer::doDeleteChunkAtPoint() {
  _mark.reset();
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
  _mark.reset();
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
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;
  return visualCol(_lines[_point.line], 0, _lines[_point.line].size()) > cw;
}

// Compute visual column width of bytes in line[from..to).
std::size_t Buffer::visualCol(std::vector<char> const& line, std::size_t from,
                              std::size_t to) const {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

// Walk a line from `from` to find the byte offset matching a target visual
// column. Stops at `to` (or line.size() if to > line.size()).
static std::size_t offsetForVisualCol(std::vector<char> const& line, std::size_t from,
                                      std::size_t to, std::size_t goal) {
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

static VisualRowInfo findVisualRow(std::vector<char> const& line, std::size_t byteOff,
                                   std::size_t cols) {
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
static std::size_t lastVisualRowStart(std::vector<char> const& line, std::size_t cols) {
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

void Buffer::moveUp(ColIdx cols) {
  auto& line = _lines[_point.line];
  // Content width excludes the gutter column.
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;
  auto vr = findVisualRow(line, _point.offset, cw);

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
    auto& prev = _lines[_point.line];
    targetRowStart = lastVisualRowStart(prev, cw);
    targetRowEnd = prev.size();
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
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;
  auto vr = findVisualRow(line, _point.offset, cw);

  // Screen column within the current visual row.
  std::size_t vcol = visualCol(line, vr.curRowStart, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  std::size_t targetRowStart, targetRowEnd;
  if (vr.nextRowStart < line.size()) {
    // Move to next visual row of the same physical line.
    targetRowStart = vr.nextRowStart;
    // Find end of next visual row.
    auto nextVr = findVisualRow(line, vr.nextRowStart, cw);
    targetRowEnd = nextVr.nextRowStart;
  } else if (_point.line + 1 < _lines.size()) {
    // Move to first visual row of next physical line.
    ++_point.line;
    auto& next = _lines[_point.line];
    targetRowStart = 0;
    if (next.empty()) {
      targetRowEnd = 0;
    } else {
      auto nextVr = findVisualRow(next, 0, cw);
      targetRowEnd = nextVr.nextRowStart;
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
  std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line == 0) {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  --_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], 0, _lines[_point.line].size(), goal);

  std::size_t actual = visualCol(_lines[_point.line], 0, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void Buffer::movePhysicalDown() {
  std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line + 1 >= _lines.size()) {
    _point.offset = _lines[_point.line].size();
    _desiredCol.reset();
    return;
  }

  ++_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], 0, _lines[_point.line].size(), goal);

  std::size_t actual = visualCol(_lines[_point.line], 0, _point.offset);
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
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
        --prev;
      if (!isSep(cur[prev])) {
        _point.offset = prev;
        goto found; // found non-sep
      }
      _point.offset = prev;
    }
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
found:

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
    _viewFrom.col = ColIdx{0};
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
    _viewFrom.col = ColIdx{0};
  }
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
  _viewFrom.col = ColIdx{0};
}

void Buffer::setPoint(std::size_t line, std::size_t offset) {
  _point.line = std::min(line, _lines.size() - 1);
  _point.offset = std::min(offset, _lines[_point.line].size());
  _desiredCol.reset();
}

void Buffer::scrollUp(bool wrap) {
  if (wrap && _viewFrom.col.value > 0) {
    _viewFrom.col = ColIdx{0};
  } else if (_viewFrom.line.value > 0) {
    _viewFrom.line = RowIdx{_viewFrom.line.value - 1};
    _viewFrom.col = ColIdx{0};
  }
}

std::size_t Buffer::screenRow(ColIdx cols, bool wrap) const {
  // Content width excludes the gutter column.
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;

  if (!wrap)
    return _point.line >= _viewFrom.line.value ? _point.line - _viewFrom.line.value : 0;

  std::size_t row = 0;
  for (std::size_t lineIdx = _viewFrom.line.value;
       lineIdx <= _point.line && lineIdx < _lines.size(); ++lineIdx) {
    auto& line = _lines[lineIdx];
    std::size_t end = (lineIdx == _point.line) ? _point.offset : line.size();
    // For the viewport line, start from the sub-line wrap offset.
    std::size_t start =
        (lineIdx == _viewFrom.line.value && wrap) ? _viewFrom.col.value : std::size_t{0};

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
    _viewFrom.col = ColIdx{0};
  }

  // Content width excludes the gutter column.
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;

  if (!wrap) {
    auto& hs = _viewFrom.col.value;

    // Vertical: simple line-based scrolling.
    if (_point.line < _viewFrom.line.value)
      _viewFrom.line = RowIdx{_point.line};
    if (_point.line >= _viewFrom.line.value + rows.value)
      _viewFrom.line = RowIdx{_point.line - rows.value + 1};

    // Horizontal: keep cursor within visible columns.
    std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
    if (vcol < hs)
      hs = vcol;
    if (vcol >= hs + cw)
      hs = vcol - cw + 1;
    return;
  }

  auto& vfo = _viewFrom.col.value;

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

  // Cheap estimate: if the point is on the viewport line and the byte
  // distance clearly exceeds the screen, skip the O(n) screenRow() check
  // and go straight to sub-line adjustment.
  bool farOffScreen = _viewFrom.line.value == _point.line && _point.offset > vfo + rows.value * cw;

  if (!farOffScreen && screenRow(cols, wrap) < rows.value)
    return;

  // Advance past lines that are entirely before the point.
  while (_viewFrom.line.value < _point.line && screenRow(cols, wrap) >= rows.value) {
    _viewFrom.line = RowIdx{_viewFrom.line.value + 1};
    vfo = 0;
  }

  if (_viewFrom.line.value != _point.line)
    return; // point is on a later line; fine-tune not needed

  // Sub-line scrolling: single scan from vfo to _point.offset,
  // keeping a ring buffer of the last `rows` visual-row start offsets.
  // At the end, the oldest entry in the ring is where the viewport should
  // start so the cursor ends up on the last visible screen row.
  auto& line = _lines[_viewFrom.line.value];
  std::size_t ringSize = rows.value;
  std::vector<std::size_t> ring(ringSize, vfo);
  std::size_t ringPos = 0;
  std::size_t col = 0;

  for (std::size_t i = vfo; i < _point.offset && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cw) {
      ringPos = (ringPos + 1) % ringSize;
      ring[ringPos] = i;
      col = 0;
    }
    col += info.width;
    if (col >= cw) {
      ringPos = (ringPos + 1) % ringSize;
      ring[ringPos] = i + info.len;
      col = 0;
    }
    i += info.len;
  }

  std::size_t readIdx = (ringPos + 1) % ringSize;
  vfo = ring[readIdx];
}

BufferPoint Buffer::viewFrom() const {
  // In wrap mode the col field is a byte offset into the first visible line;
  // in no-wrap mode horizontal scroll does not affect line/byte comparisons.
  std::size_t off = _wrapMode ? _viewFrom.col.value : 0;
  return {_viewFrom.line.value, off};
}

std::tuple<std::size_t, std::size_t> Buffer::lineCol() const {
  // Line is 1-indexed.
  std::size_t line = _point.line + 1;
  // Column counts code points, not bytes.
  std::size_t col = 1;
  auto& curLine = _lines[_point.line];
  for (std::size_t i = 0; i < _point.offset && i < curLine.size();) {
    auto len = utf8SeqLen(static_cast<unsigned char>(curLine[i]));
    ++col;
    i += std::min(len, curLine.size() - i);
  }
  return {line, col};
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

  std::string result;
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
  if (to < from)
    std::swap(from, to);
  if (from.line >= _lines.size())
    return;

  to.line = std::min(to.line, _lines.size() - 1);
  to.offset = std::min(to.offset, _lines[to.line].size());
  from.offset = std::min(from.offset, _lines[from.line].size());

  // Splice: head of from-line + tail of to-line.
  auto& fromLine = _lines[from.line];
  auto& toLine = _lines[to.line];
  std::vector<char> merged(fromLine.begin(), fromLine.begin() + from.offset);
  merged.insert(merged.end(), toLine.begin() + to.offset, toLine.end());

  // Erase lines [from.line .. to.line], replace with merged.
  _lines.erase(_lines.begin() + from.line, _lines.begin() + to.line + 1);
  _lines.insert(_lines.begin() + from.line, std::move(merged));

  _point = from;
  _mark.reset();
  _modified = true;
}

BufferPoint Buffer::insertText(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      // Split current line at _point.
      auto& line = _lines[_point.line];
      std::vector<char> tail(line.begin() + _point.offset, line.end());
      line.erase(line.begin() + _point.offset, line.end());
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
  _mark.reset();
  _modified = true;
  return _point;
}

std::tuple<RowIdx, ColIdx> Buffer::render(std::string& out, RowIdx rows, ColIdx cols,
                                          bool showWhitespace, bool wrap) const {
  static constexpr auto kCtrlColor = "\x1b[38;2;204;120;50m";
  static constexpr auto kReset = "\x1b[0m";
  static constexpr auto kWsFg = "\x1b[38;2;140;60;60m";
  static constexpr auto kSpaceStyle = "\x1b[48;2;45;45;45m";
  static constexpr auto kTabWsStyle = "\x1b[48;2;60;60;60m";
  static constexpr auto kTabStyle = "\x1b[48;2;70;70;70m";
  static constexpr auto kNlStyle = "\x1b[48;2;35;35;35m";

  auto emitRowStart = [&](std::size_t r) {
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";1H";
  };

  auto emitChar = [&](char const* data, CharInfo info) {
    auto u = static_cast<unsigned char>(*data);
    if (u < 0x80) {
      if (*data == '\t') {
        if (showWhitespace) {
          out += kTabWsStyle;
          out += kWsFg;
          out += "\xe2\x96\xa0\xe2\x96\xba";
          out += kReset;
        } else {
          out += kTabStyle;
          out += "  ";
          out += kReset;
        }
      } else if (*data == ' ' && showWhitespace) {
        out += kSpaceStyle;
        out += kWsFg;
        out += "\xc2\xb7";
        out += kReset;
      } else if (u == 0) {
        out += kCtrlColor;
        out += "\\0";
        out += kReset;
      } else if (u == 0x7f) {
        out += kCtrlColor;
        out += "^?";
        out += kReset;
      } else if (u < 0x20) {
        out += kCtrlColor;
        out += '^';
        out += static_cast<char>('@' + u);
        out += kReset;
      } else {
        out += *data;
      }
    } else if (info.len == 1) {
      out += kCtrlColor;
      auto nibble = [](unsigned char n) -> char { return n < 10 ? ('0' + n) : ('a' + n - 10); };
      out += nibble(u >> 4);
      out += nibble(u & 0xf);
      out += kReset;
    } else {
      out.append(data, info.len);
    }
  };

  // Content width excludes the gutter column.
  std::size_t cw = cols.value > 1 ? cols.value - 1 : 1;

  // Gutter styles: reverse video matching the status bar.
  static constexpr auto kGutter = "\x1b[7m";
  static constexpr auto kGutterHi = "\x1b[0m"; // non-reverse for cursor line

  // Emit the gutter character at column cols.value for a given row.
  // `highlight` marks the visual row containing the cursor.
  auto emitGutter = [&](std::size_t r, bool highlight, char const* indicator) {
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";";
    out += std::to_string(cols.value);
    out += "H";
    out += highlight ? kGutterHi : kGutter;
    out += indicator;
    out += kReset;
  };

  // Region highlighting: compute normalized start/end from mark and point.
  static constexpr auto kRegionOn = "\x1b[7m";
  static constexpr auto kRegionOff = "\x1b[0m";
  bool hasRegion = _mark.has_value();
  BufferPoint regionStart, regionEnd;
  if (hasRegion) {
    regionStart = std::min(_mark.value(), _point);
    regionEnd = std::max(_mark.value(), _point);
  }

  // Find highlight (light blue background).
  static constexpr auto kHighlightOn = "\x1b[48;2;60;100;160m";
  static constexpr auto kHighlightOff = "\x1b[0m";

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
  std::size_t hScroll = wrap ? std::size_t{0} : _viewFrom.col.value;
  std::size_t viewFromOffset = wrap ? _viewFrom.col.value : std::size_t{0};

  if (!wrap) {
    // No-wrap: each physical line is one screen row, horizontally scrolled.
    std::size_t row = 0;
    emitRowStart(0);

    for (std::size_t lineIdx = _viewFrom.line.value; lineIdx < _lines.size() && row < rows.value;
         ++lineIdx) {
      auto& line = _lines[lineIdx];
      std::size_t vcol = 0;
      bool overflows = false; // content extends past visible area
      bool regionActive = false;
      bool highlightActive = false;

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

        bool wantHl = inHighlight(lineIdx, i);
        bool wantRegion = !wantHl && inRegion(lineIdx, i);
        if (wantHl && !highlightActive) {
          if (regionActive) {
            out += kRegionOff;
            regionActive = false;
          }
          out += kHighlightOn;
          highlightActive = true;
        } else if (!wantHl && highlightActive) {
          out += kHighlightOff;
          highlightActive = false;
        }
        if (wantRegion && !regionActive) {
          out += kRegionOn;
          regionActive = true;
        } else if (!wantRegion && regionActive) {
          out += kRegionOff;
          regionActive = false;
        }

        emitChar(line.data() + i, info);
        vcol += info.width;
        i += info.len;
      }

      if (lineIdx == _point.line && _point.offset >= line.size() && !pointFound) {
        cur_row = row;
        cur_col = vcol >= hScroll ? vcol - hScroll : 0;
        pointFound = true;
      }

      // Highlight end-of-line newline position if region spans it.
      if (hasRegion && !overflows && lineIdx + 1 < _lines.size() && vcol >= hScroll &&
          vcol < hScroll + cw && inRegion(lineIdx, line.size())) {
        if (!regionActive) {
          out += kRegionOn;
          regionActive = true;
        }
        out += ' ';
      }

      if (highlightActive) {
        out += kHighlightOff;
        highlightActive = false;
      }
      if (regionActive) {
        out += kRegionOff;
        regionActive = false;
      }

      // Newline indicator.
      if (showWhitespace && !overflows && lineIdx + 1 < _lines.size() && vcol >= hScroll &&
          vcol < hScroll + cw && !inRegion(lineIdx, line.size())) {
        out += kNlStyle;
        out += kWsFg;
        out += "\xe2\x86\xb5";
        out += kReset;
      }

      out += "\x1b[K";

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
      out += "\x1b[K";
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
    bool regionActive = false;
    bool highlightActive = false;

    for (std::size_t i = startByte; i < line.size() && row < rows.value;) {
      auto info = charInfoAt(line.data() + i, line.size() - i);

      // Wrap if the representation doesn't fit on the current line.
      if (col + info.width > cw || pendingWrap) {
        if (highlightActive) {
          out += kHighlightOff;
          highlightActive = false;
        }
        if (regionActive) {
          out += kRegionOff;
          regionActive = false;
        }
        out += "\x1b[K";
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

      bool wantHl = inHighlight(lineIdx, i);
      bool wantRegion = !wantHl && inRegion(lineIdx, i);
      if (wantHl && !highlightActive) {
        if (regionActive) {
          out += kRegionOff;
          regionActive = false;
        }
        out += kHighlightOn;
        highlightActive = true;
      } else if (!wantHl && highlightActive) {
        out += kHighlightOff;
        highlightActive = false;
      }
      if (wantRegion && !regionActive) {
        out += kRegionOn;
        regionActive = true;
      } else if (!wantRegion && regionActive) {
        out += kRegionOff;
        regionActive = false;
      }

      emitChar(line.data() + i, info);
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
      // Highlight end-of-line newline position if region spans it.
      if (hasRegion && lineIdx + 1 < _lines.size() && col + 1 <= cw &&
          inRegion(lineIdx, line.size())) {
        if (!regionActive) {
          out += kRegionOn;
          regionActive = true;
        }
        out += ' ';
      }
      if (highlightActive) {
        out += kHighlightOff;
        highlightActive = false;
      }
      if (regionActive) {
        out += kRegionOff;
        regionActive = false;
      }
      if (showWhitespace && lineIdx + 1 < _lines.size() && col + 1 <= cw &&
          !inRegion(lineIdx, line.size())) {
        out += kNlStyle;
        out += kWsFg;
        out += "\xe2\x86\xb5"; // down-arrow
        out += kReset;
      }
      out += "\x1b[K";
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
    out += "\x1b[K";
    emitGutter(r, false, " ");
  }

  return {RowIdx{cur_row}, ColIdx{cur_col}};
}
