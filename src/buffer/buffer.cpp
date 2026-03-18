#include "buffer.hpp"
#include "char_width.hpp"

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

void Buffer::doInsert(uint32_t cp) {
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

  auto &line = _lines[_point.line];
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
  auto &line = _lines[_point.line];
  if (_point.offset < line.size()) {
    // Delete within line.
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    auto end = std::min(_point.offset + len, line.size());
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
  } else if (_point.line + 1 < _lines.size()) {
    // At end of line — merge next line into this one (delete the newline).
    auto &next = _lines[_point.line + 1];
    line.insert(line.end(), next.begin(), next.end());
    _lines.erase(_lines.begin() + _point.line + 1);
    _modified = true;
  }
}

void Buffer::doDeleteBeforePoint() {
  auto &line = _lines[_point.line];
  if (_point.offset > 0) {
    // Walk back over continuation bytes.
    auto end = _point.offset;
    --_point.offset;
    while (_point.offset > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
    line.erase(line.begin() + _point.offset, line.begin() + end);
    _modified = true;
  } else if (_point.line > 0) {
    // At start of line — merge into end of previous line (delete the newline).
    auto &prev = _lines[_point.line - 1];
    std::size_t newOffset = prev.size();
    prev.insert(prev.end(), line.begin(), line.end());
    _lines.erase(_lines.begin() + _point.line);
    _point.line--;
    _point.offset = newOffset;
    _modified = true;
  }
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
    auto &line = _lines[_point.line];
    --_point.offset;
    while (_point.offset > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _lines[_point.line].size();
  }
  _desiredCol.reset();
}

void Buffer::moveRight() {
  auto &line = _lines[_point.line];
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  } else if (_point.line + 1 < _lines.size()) {
    ++_point.line;
    _point.offset = 0;
  }
  _desiredCol.reset();
}

// Compute visual column width of bytes in line[from..to).
std::size_t Buffer::visualCol(std::vector<char> const &line, std::size_t from,
                              std::size_t to) const {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

// Walk a line to find the byte offset matching a target visual column.
static std::size_t offsetForVisualCol(std::vector<char> const &line,
                                      std::size_t goal) {
  std::size_t pos = 0, v = 0;
  while (pos < line.size()) {
    auto info = charInfoAt(line.data() + pos, line.size() - pos);
    if (v + info.width > goal)
      break;
    v += info.width;
    pos += info.len;
  }
  return pos;
}

void Buffer::moveUp() {
  std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line == 0) {
    _point.offset = 0;
    _desiredCol.reset();
    return;
  }

  --_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], goal);

  std::size_t actual = visualCol(_lines[_point.line], 0, _point.offset);
  if (actual == goal)
    _desiredCol.reset();
  else if (!_desiredCol)
    _desiredCol = ColIdx{goal};
}

void Buffer::moveDown() {
  std::size_t vcol = visualCol(_lines[_point.line], 0, _point.offset);
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  if (_point.line + 1 >= _lines.size()) {
    _point.offset = _lines[_point.line].size();
    _desiredCol.reset();
    return;
  }

  ++_point.line;
  _point.offset = offsetForVisualCol(_lines[_point.line], goal);

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
    if (c != ' ' && c != '\t')
      return false;
  return true;
}

void Buffer::moveWordRight() {
  auto &line = _lines[_point.line];
  // At very end of buffer?
  if (_point.line == _lines.size() - 1 && _point.offset >= line.size()) {
    _desiredCol.reset();
    return;
  }

  auto isSep = [](char c) { return c == ' ' || c == '\t'; };

  // Step forward one code point (or cross to next line).
  if (_point.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[_point.offset]));
    _point.offset = std::min(_point.offset + len, line.size());
  }
  // If at end of line, cross to next.
  if (_point.offset >= _lines[_point.line].size() &&
      _point.line + 1 < _lines.size()) {
    ++_point.line;
    _point.offset = 0;
  }

  // Skip separators and line boundaries forward.
  for (;;) {
    auto &cur = _lines[_point.line];
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
    auto &cur = _lines[_point.line];
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

  auto isSep = [](char c) { return c == ' ' || c == '\t'; };

  // Step back one code point (or cross to previous line).
  if (_point.offset > 0) {
    auto &line = _lines[_point.line];
    --_point.offset;
    while (_point.offset > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(line[_point.offset])))
      --_point.offset;
  } else if (_point.line > 0) {
    --_point.line;
    _point.offset = _lines[_point.line].size();
    // We're now at the "newline" position. Step back once more if possible.
    if (_point.offset > 0) {
      auto &line = _lines[_point.line];
      --_point.offset;
      while (_point.offset > 0 && isUtf8Continuation(static_cast<unsigned char>(
                                      line[_point.offset])))
        --_point.offset;
    }
  }

  // Skip separators and line boundaries backward.
  for (;;) {
    auto &cur = _lines[_point.line];
    if (_point.offset < cur.size() && !isSep(cur[_point.offset]))
      break; // on a non-sep char
    // Skip seps backward within line.
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 &&
             isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
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
      auto &prev = _lines[_point.line];
      _point.offset = prev.size();
      if (_point.offset > 0) {
        --_point.offset;
        while (
            _point.offset > 0 &&
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
    auto &cur = _lines[_point.line];
    if (_point.offset < cur.size() && isSep(cur[_point.offset])) {
      _desiredCol.reset();
      return;
    }
  }

  // Walk backward to the first char of this word.
  {
    auto &cur = _lines[_point.line];
    while (_point.offset > 0) {
      std::size_t prev = _point.offset - 1;
      while (prev > 0 &&
             isUtf8Continuation(static_cast<unsigned char>(cur[prev])))
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

void Buffer::pageUp(RowIdx screenRows) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  for (std::size_t i = 0; i < half; ++i) {
    moveUp();
    scrollUp();
  }
}

void Buffer::pageDown(RowIdx screenRows) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  for (std::size_t i = 0; i < half; ++i) {
    moveDown();
    scrollDown();
  }
  // Clamp viewport so at least half a screen of content is visible.
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFromLine > limit) {
    _viewFromLine = limit;
    _viewFromOffset = 0;
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
  if (_viewFromLine < limit)
    scrollDown();
}

void Buffer::scrollDown() {
  if (_viewFromLine + 1 < _lines.size()) {
    ++_viewFromLine;
    _viewFromOffset = 0;
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

  // Center _viewFromLine so the target line is in the middle of the screen.
  std::size_t half = screenRows.value / 2;
  if (idx >= half)
    _viewFromLine = idx - half;
  else
    _viewFromLine = 0;
  _viewFromOffset = 0;
}

void Buffer::scrollUp() {
  if (_viewFromOffset > 0) {
    _viewFromOffset = 0;
  } else if (_viewFromLine > 0) {
    --_viewFromLine;
    _viewFromOffset = 0;
  }
}

std::size_t Buffer::screenRow(ColIdx cols) const {
  std::size_t row = 0;
  for (std::size_t lineIdx = _viewFromLine;
       lineIdx <= _point.line && lineIdx < _lines.size(); ++lineIdx) {
    auto &line = _lines[lineIdx];
    std::size_t end = (lineIdx == _point.line) ? _point.offset : line.size();
    // For the viewport line, start from _viewFromOffset (sub-line scroll).
    std::size_t start =
        (lineIdx == _viewFromLine) ? _viewFromOffset : std::size_t{0};

    std::size_t col = 0;
    for (std::size_t i = start; i < end && i < line.size();) {
      auto info = charInfoAt(line.data() + i, line.size() - i);
      if (col + info.width > cols.value) {
        ++row;
        col = 0;
      }
      col += info.width;
      if (col >= cols.value) {
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

void Buffer::ensureVisible(RowIdx rows, ColIdx cols) {
  // Handle point above the viewport (including sub-line offset).
  if (_point < viewFrom()) {
    _viewFromLine = _point.line;
    _viewFromOffset = 0;
  }

  // Handle point far below the viewport (line-level jump).
  if (_point.line >= _viewFromLine + rows.value) {
    std::size_t half = rows.value / 2;
    _viewFromLine = _point.line >= half ? _point.line - half : 0;
    _viewFromOffset = 0;
  }

  // Cheap estimate: if the point is on the viewport line and the byte
  // distance clearly exceeds the screen, skip the O(n) screenRow() check
  // and go straight to sub-line adjustment.
  bool farOffScreen = _viewFromLine == _point.line &&
                      _point.offset > _viewFromOffset + rows.value * cols.value;

  if (!farOffScreen && screenRow(cols) < rows.value)
    return;

  // Advance past lines that are entirely before the point.
  while (_viewFromLine < _point.line && screenRow(cols) >= rows.value) {
    ++_viewFromLine;
    _viewFromOffset = 0;
  }

  if (_viewFromLine != _point.line)
    return; // point is on a later line; fine-tune not needed

  // Sub-line scrolling: single scan from _viewFromOffset to _point.offset,
  // keeping a ring buffer of the last `rows` visual-row start offsets.
  // At the end, the oldest entry in the ring is where the viewport should
  // start so the cursor ends up on the last visible screen row.
  auto &line = _lines[_viewFromLine];
  std::size_t ringSize = rows.value;
  std::vector<std::size_t> ring(ringSize, _viewFromOffset);
  std::size_t ringPos = 0;
  std::size_t col = 0;

  for (std::size_t i = _viewFromOffset; i < _point.offset && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols.value) {
      ringPos = (ringPos + 1) % ringSize;
      ring[ringPos] = i;
      col = 0;
    }
    col += info.width;
    if (col >= cols.value) {
      ringPos = (ringPos + 1) % ringSize;
      ring[ringPos] = i + info.len;
      col = 0;
    }
    i += info.len;
  }

  std::size_t readIdx = (ringPos + 1) % ringSize;
  _viewFromOffset = ring[readIdx];
}

std::tuple<std::size_t, std::size_t> Buffer::lineCol() const {
  // Line is 1-indexed.
  std::size_t line = _point.line + 1;
  // Column counts code points, not bytes.
  std::size_t col = 1;
  auto &curLine = _lines[_point.line];
  for (std::size_t i = 0; i < _point.offset && i < curLine.size();) {
    auto len = utf8SeqLen(static_cast<unsigned char>(curLine[i]));
    ++col;
    i += std::min(len, curLine.size() - i);
  }
  return {line, col};
}

std::tuple<RowIdx, ColIdx> Buffer::render(std::string &out, RowIdx rows,
                                          ColIdx cols,
                                          bool showWhitespace) const {
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

  std::size_t row = 0, col = 0;
  std::size_t cur_row = 0, cur_col = 0;
  bool pointFound = false;

  emitRowStart(0);

  for (std::size_t lineIdx = _viewFromLine;
       lineIdx < _lines.size() && row < rows.value; ++lineIdx) {
    auto &line = _lines[lineIdx];
    // For the viewport line, start from _viewFromOffset (sub-line scroll).
    std::size_t startByte =
        (lineIdx == _viewFromLine) ? _viewFromOffset : std::size_t{0};

    for (std::size_t i = startByte; i < line.size() && row < rows.value;) {
      char ch = line[i];
      auto u = static_cast<unsigned char>(ch);
      auto info = charInfoAt(line.data() + i, line.size() - i);

      // Wrap if the representation doesn't fit on the current line.
      if (col + info.width > cols.value) {
        out += "\x1b[K";
        ++row;
        col = 0;
        if (row >= rows.value)
          break;
        emitRowStart(row);
      }

      if (lineIdx == _point.line && i == _point.offset && !pointFound) {
        cur_row = row;
        cur_col = col;
        pointFound = true;
      }

      if (u < 0x80) {
        if (ch == '\t') {
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
        } else if (ch == ' ' && showWhitespace) {
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
          out += ch;
        }
      } else if (info.len == 1) {
        out += kCtrlColor;
        auto nibble = [](unsigned char n) -> char {
          return n < 10 ? ('0' + n) : ('a' + n - 10);
        };
        out += nibble(u >> 4);
        out += nibble(u & 0xf);
        out += kReset;
      } else {
        out.append(line.data() + i, info.len);
      }
      col += info.width;

      if (col >= cols.value) {
        out += "\x1b[K";
        ++row;
        col = 0;
        if (row < rows.value)
          emitRowStart(row);
      }

      i += info.len;
    }

    // Point at end of line (offset == line.size()).
    if (lineIdx == _point.line && _point.offset >= line.size() && !pointFound) {
      cur_row = row;
      cur_col = col;
      pointFound = true;
    }

    // Emit newline visual if not the last line.
    if (row < rows.value) {
      if (showWhitespace && lineIdx + 1 < _lines.size() &&
          col + 1 <= cols.value) {
        out += kNlStyle;
        out += kWsFg;
        out += "\xe2\x86\xb5"; // down-arrow
        out += kReset;
      }
      out += "\x1b[K";
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
  }

  return {RowIdx{cur_row}, ColIdx{cur_col}};
}
