#include "buffer.hpp"
#include "char_width.hpp"

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

std::size_t Buffer::lineStart(std::size_t pos) const {
  while (pos > 0 && _data[pos - 1] != '\n')
    --pos;
  return pos;
}

// Performance note: insertion into a vector<char> is O(n) per call because
// all data after _point must be shifted. A gap buffer or rope would make
// insertions O(1) amortised, but the current approach is acceptable for
// typical file sizes.
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
  _data.insert(_data.begin() + _point, buf, buf + n);
  _point += n;
  _modified = true;

  if (_desiredCol) {
    std::size_t ls = lineStart(_point);
    std::size_t vcol = 0;
    for (std::size_t i = ls; i < _point;) {
      auto info = charInfoAt(_data.data() + i, _point - i);
      vcol += info.width;
      i += info.len;
    }
    if (vcol >= _desiredCol->value)
      _desiredCol.reset();
  }
}

void Buffer::doDeleteAtPoint() {
  if (_point < _data.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(_data[_point]));
    auto end = std::min(_point + len, _data.size());
    _data.erase(_data.begin() + _point, _data.begin() + end);
    _modified = true;
  }
}

void Buffer::doDeleteBeforePoint() {
  if (_point > 0) {
    // Walk back over continuation bytes to find the start of the code point.
    auto end = _point;
    --_point;
    while (_point > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
      --_point;
    // Delete everything from the start we found to the original position.
    // This correctly handles malformed data (stray continuation bytes).
    _data.erase(_data.begin() + _point, _data.begin() + end);
    _modified = true;
  }
}

void Buffer::moveHome() {
  _point = lineStart(_point);
  _desiredCol.reset();
}

void Buffer::moveEnd() {
  while (_point < _data.size() && _data[_point] != '\n')
    ++_point;
  _desiredCol.reset();
}

void Buffer::moveLeft() {
  if (_point > 0) {
    --_point;
    // Skip back over UTF-8 continuation bytes to the start of the code point.
    while (_point > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
      --_point;
  }
  _desiredCol.reset();
}

void Buffer::moveRight() {
  if (_point < _data.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(_data[_point]));
    _point = std::min(_point + len, _data.size());
  }
  _desiredCol.reset();
}

// Compute visual column width of text in _data[from..to).
std::size_t Buffer::visualCol(std::size_t from, std::size_t to) const {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to;) {
    auto info = charInfoAt(_data.data() + i, to - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

void Buffer::moveUp() {
  // Find start of current line and compute visual column.
  std::size_t ls = lineStart(_point);
  std::size_t vcol = visualCol(ls, _point);

  // The goal column is the desired column if set, otherwise the current one.
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  // If already on the first line, move to beginning.
  if (ls == 0) {
    _point = 0;
    _desiredCol.reset();
    return;
  }

  // Find start of previous line.
  std::size_t prev_line_end = ls - 1; // the '\n' before current line
  std::size_t prev_line_start = lineStart(prev_line_end);

  // Walk the previous line to find the position matching the goal visual
  // column.
  std::size_t pos = prev_line_start;
  std::size_t v = 0;
  while (pos < prev_line_end) {
    auto info = charInfoAt(_data.data() + pos, prev_line_end - pos);
    if (v + info.width > goal)
      break;
    v += info.width;
    pos += info.len;
  }
  _point = pos;

  if (v == goal) {
    _desiredCol.reset();
  } else if (!_desiredCol) {
    _desiredCol = ColIdx{goal};
  }
}

void Buffer::moveDown() {
  // Find start of current line and compute visual column.
  std::size_t ls = lineStart(_point);
  std::size_t vcol = visualCol(ls, _point);

  // The goal column is the desired column if set, otherwise the current one.
  std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

  // Find start of next line (past the '\n' after current position).
  std::size_t next_line_start = _point;
  while (next_line_start < _data.size() && _data[next_line_start] != '\n')
    ++next_line_start;

  // If no newline found, move to end.
  if (next_line_start >= _data.size()) {
    _point = _data.size();
    _desiredCol.reset();
    return;
  }

  ++next_line_start; // skip the '\n'

  // Find end of next line.
  std::size_t next_line_end = next_line_start;
  while (next_line_end < _data.size() && _data[next_line_end] != '\n')
    ++next_line_end;

  // Walk the next line to find the position matching the goal visual column.
  std::size_t pos = next_line_start;
  std::size_t v = 0;
  while (pos < next_line_end) {
    auto info = charInfoAt(_data.data() + pos, next_line_end - pos);
    if (v + info.width > goal)
      break;
    v += info.width;
    pos += info.len;
  }
  _point = pos;

  if (v == goal) {
    _desiredCol.reset();
  } else if (!_desiredCol) {
    _desiredCol = ColIdx{goal};
  }
}

bool Buffer::isBlankLine(std::size_t ls) const {
  for (std::size_t i = ls; i < _data.size() && _data[i] != '\n'; ++i)
    if (_data[i] != ' ' && _data[i] != '\t')
      return false;
  return true;
}

void Buffer::moveWordRight() {
  if (_point >= _data.size()) {
    _desiredCol.reset();
    return;
  }

  auto isSep = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };

  // Step forward one code point.
  auto len = utf8SeqLen(static_cast<unsigned char>(_data[_point]));
  _point = std::min(_point + len, _data.size());

  // Skip separators forward.
  while (_point < _data.size() && isSep(_data[_point]))
    ++_point;

  // Advance to the last char of this word.
  while (_point < _data.size()) {
    len = utf8SeqLen(static_cast<unsigned char>(_data[_point]));
    auto next = std::min(_point + len, _data.size());
    if (next >= _data.size() || isSep(_data[next]))
      break;
    _point = next;
  }
  _desiredCol.reset();
}

void Buffer::moveWordLeft() {
  if (_point == 0) {
    _desiredCol.reset();
    return;
  }

  auto isSep = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };

  // Step back one code point.
  --_point;
  while (_point > 0 &&
         isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
    --_point;

  // Skip separators backward.
  while (_point > 0 && isSep(_data[_point])) {
    --_point;
    while (_point > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
      --_point;
  }

  // If stuck on a separator at position 0, done.
  if (isSep(_data[_point])) {
    _desiredCol.reset();
    return;
  }

  // Walk backward to the first char of this word.
  while (_point > 0) {
    std::size_t prev = _point - 1;
    while (prev > 0 &&
           isUtf8Continuation(static_cast<unsigned char>(_data[prev])))
      --prev;
    if (isSep(_data[prev]))
      break;
    _point = prev;
  }
  _desiredCol.reset();
}

void Buffer::moveParagraphUp() {
  std::size_t ls = lineStart(_point);
  if (ls == 0) {
    _point = 0;
    _desiredCol.reset();
    return;
  }

  // Go to the line above.
  std::size_t pos = lineStart(ls - 1);

  // Skip blank lines upward.
  while (pos > 0 && isBlankLine(pos))
    pos = lineStart(pos - 1);

  // If at top and still blank, go to start.
  if (isBlankLine(pos)) {
    _point = 0;
    _desiredCol.reset();
    return;
  }

  // On a non-blank line. Find the start of this paragraph.
  while (pos > 0) {
    std::size_t prev = lineStart(pos - 1);
    if (isBlankLine(prev))
      break;
    pos = prev;
  }

  _point = pos;
  _desiredCol.reset();
}

void Buffer::moveParagraphDown() {
  std::size_t ls = lineStart(_point);
  bool onBlank = isBlankLine(ls);

  // Advance to the next line.
  std::size_t pos = _point;
  while (pos < _data.size() && _data[pos] != '\n')
    ++pos;
  if (pos < _data.size())
    ++pos;

  // If current line was non-blank, skip non-blank lines (rest of paragraph).
  if (!onBlank) {
    while (pos < _data.size() && !isBlankLine(pos)) {
      while (pos < _data.size() && _data[pos] != '\n')
        ++pos;
      if (pos < _data.size())
        ++pos;
    }
  }

  // Skip blank lines.
  while (pos < _data.size() && isBlankLine(pos)) {
    while (pos < _data.size() && _data[pos] != '\n')
      ++pos;
    if (pos < _data.size())
      ++pos;
  }

  _point = std::min(pos, _data.size());
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
  if (_viewFrom > limit)
    _viewFrom = limit;
}

std::size_t Buffer::viewFromLinesBeforeEnd(std::size_t lines) const {
  std::size_t pos = _data.size();
  for (std::size_t i = 0; i < lines && pos > 0; ++i) {
    --pos;
    while (pos > 0 && _data[pos - 1] != '\n')
      --pos;
  }
  return pos;
}

void Buffer::scrollViewNearEnd(RowIdx screenRows) {
  std::size_t half = screenRows.value / 2;
  if (half == 0)
    half = 1;
  std::size_t limit = viewFromLinesBeforeEnd(half);
  if (_viewFrom < limit)
    scrollDown();
}

void Buffer::scrollDown() {
  // Find the next newline. If found, advance past it. If the last line
  // has no trailing newline, stay put rather than advancing to the end.
  std::size_t pos = _viewFrom;
  while (pos < _data.size() && _data[pos] != '\n')
    ++pos;
  if (pos < _data.size())
    _viewFrom = pos + 1; // skip the '\n'
}

void Buffer::gotoLine(std::size_t line, RowIdx screenRows) {
  // Move _point to the start of the 1-based line number.
  // If line exceeds the line count, move to the last line.
  if (line == 0)
    line = 1;

  _point = 0;
  for (std::size_t cur = 1; cur < line && _point < _data.size(); ++cur) {
    while (_point < _data.size() && _data[_point] != '\n')
      ++_point;
    if (_point < _data.size())
      ++_point; // skip the '\n'
  }

  // Center _viewFrom so the target line is in the middle of the screen.
  // First, count how many lines back we need to go (half the screen).
  std::size_t half = screenRows.value / 2;
  _viewFrom = _point;
  for (std::size_t i = 0; i < half && _viewFrom > 0; ++i) {
    --_viewFrom; // step back over '\n' or into previous line
    while (_viewFrom > 0 && _data[_viewFrom - 1] != '\n')
      --_viewFrom;
  }
}

void Buffer::scrollUp() {
  if (_viewFrom == 0)
    return;
  // _viewFrom is at the start of a line. Step back over the '\n' that
  // ends the previous line, then find the start of that line.
  --_viewFrom; // now on the '\n' of the previous line
  while (_viewFrom > 0 && _data[_viewFrom - 1] != '\n')
    --_viewFrom;
}

// Performance note: scans from _viewFrom to _point on every call. This is
// O(n) per keystroke when the cursor is far from _viewFrom. Called once per
// draw(), so acceptable for now but a cached line index would improve this.
std::size_t Buffer::screenRow(ColIdx cols) const {
  std::size_t row = 0, col = 0;
  for (std::size_t i = _viewFrom; i < _point && i < _data.size();) {
    if (_data[i] == '\n') {
      ++row;
      col = 0;
      ++i;
    } else {
      auto info = charInfoAt(_data.data() + i, _data.size() - i);
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
  }
  return row;
}

// Performance note: scans from _data[0] to _point on every call (O(n) per
// keystroke). Called once per draw() for the status bar. A cached line/col
// updated incrementally on each edit would improve this.
std::tuple<std::size_t, std::size_t> Buffer::lineCol() const {
  std::size_t line = 1, col = 1;
  for (std::size_t i = 0; i < _point && i < _data.size();) {
    if (_data[i] == '\n') {
      ++line;
      col = 1;
      ++i;
    } else {
      auto len = utf8SeqLen(static_cast<unsigned char>(_data[i]));
      ++col;
      i += std::min(len, _data.size() - i);
    }
  }
  return {line, col};
}

// Performance note: rebuilds the entire ANSI output from scratch on every
// call. The `out` string is reused (clear() keeps capacity), so allocation
// pressure is bounded, but the CPU work scales with screen size. This is
// not a performance concern at typical terminal sizes.
std::tuple<RowIdx, ColIdx> Buffer::render(std::string &out, RowIdx rows,
                                          ColIdx cols,
                                          bool showWhitespace) const {
  static constexpr auto kCtrlColor = "\x1b[38;2;204;120;50m";
  static constexpr auto kReset = "\x1b[0m";
  // Whitespace-mode styles: background + dark red foreground.
  static constexpr auto kWsFg = "\x1b[38;2;140;60;60m";
  static constexpr auto kSpaceStyle =
      "\x1b[48;2;45;45;45m"; // very light grey bg
  static constexpr auto kTabWsStyle = "\x1b[48;2;60;60;60m"; // darker grey bg
  static constexpr auto kTabStyle = "\x1b[48;2;70;70;70m";   // normal tab bg
  static constexpr auto kNlStyle = "\x1b[48;2;35;35;35m"; // very dark grey bg

  // Emit cursor positioning for the start of each row.
  auto emitRowStart = [&](std::size_t r) {
    out += "\x1b[";
    out += std::to_string(r + 1);
    out += ";1H";
  };

  std::size_t row = 0, col = 0;
  std::size_t cur_row = 0, cur_col = 0;

  emitRowStart(0);

  for (std::size_t i = _viewFrom; i < _data.size() && row < rows.value;) {
    char ch = _data[i];

    if (ch == '\n') {
      if (i == _point) {
        cur_row = row;
        cur_col = col;
      }
      if (showWhitespace && col + 1 <= cols.value && row < rows.value) {
        out += kNlStyle;
        out += kWsFg;
        out += "\xe2\x86\xb5"; // ↵
        out += kReset;
      }
      out += "\x1b[K";
      ++row;
      col = 0;
      if (row < rows.value)
        emitRowStart(row);
      ++i;
      continue;
    }

    auto u = static_cast<unsigned char>(ch);
    auto info = charInfoAt(_data.data() + i, _data.size() - i);

    // Wrap if the representation doesn't fit on the current line.
    if (col + info.width > cols.value) {
      out += "\x1b[K";
      ++row;
      col = 0;
      if (row >= rows.value)
        break;
      emitRowStart(row);
    }

    if (i == _point) {
      cur_row = row;
      cur_col = col;
    }

    if (u < 0x80) {
      // ASCII: handle control characters specially.
      if (ch == '\t') {
        if (showWhitespace) {
          out += kTabWsStyle;
          out += kWsFg;
          out += "\xe2\x96\xa0\xe2\x96\xba"; // ■►
          out += kReset;
        } else {
          out += kTabStyle;
          out += "  ";
          out += kReset;
        }
      } else if (ch == ' ' && showWhitespace) {
        out += kSpaceStyle;
        out += kWsFg;
        out += "\xc2\xb7"; // ·
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
      // Invalid byte — display as 2-digit hex.
      out += kCtrlColor;
      auto nibble = [](unsigned char n) -> char {
        return n < 10 ? ('0' + n) : ('a' + n - 10);
      };
      out += nibble(u >> 4);
      out += nibble(u & 0xf);
      out += kReset;
    } else {
      // Valid multi-byte UTF-8: emit the whole sequence.
      out.append(_data.data() + i, info.len);
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

  // If point is at or past the end of data, its position is wherever
  // the scan ended up.
  if (_point >= _data.size()) {
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
