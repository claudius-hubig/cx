#include "buffer.hpp"
#include "char_width.hpp"

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

void Buffer::doInsert(char c) {
    if (_point >= _data.size())
        _data.push_back(c);
    else
        _data.insert(_data.begin() + _point, c);
    ++_point;

    if (_desiredCol) {
        // Compute visual column after the insertion.
        std::size_t line_start = _point;
        while (line_start > 0 && _data[line_start - 1] != '\n')
            --line_start;
        std::size_t vcol = 0;
        for (std::size_t i = line_start; i < _point;) {
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
    }
}

void Buffer::doDeleteBeforePoint() {
    if (_point > 0) {
        // Walk back over continuation bytes to find the start of the code point.
        --_point;
        while (_point > 0 && isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
            --_point;
        auto len = utf8SeqLen(static_cast<unsigned char>(_data[_point]));
        auto end = std::min(_point + len, _data.size());
        _data.erase(_data.begin() + _point, _data.begin() + end);
    }
}

void Buffer::moveHome() {
    while (_point > 0 && _data[_point - 1] != '\n')
        --_point;
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
        while (_point > 0 && isUtf8Continuation(static_cast<unsigned char>(_data[_point])))
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
    std::size_t line_start = _point;
    while (line_start > 0 && _data[line_start - 1] != '\n')
        --line_start;
    std::size_t vcol = visualCol(line_start, _point);

    // The goal column is the desired column if set, otherwise the current one.
    std::size_t goal = _desiredCol ? _desiredCol->value : vcol;

    // If already on the first line, move to beginning.
    if (line_start == 0) {
        _point = 0;
        _desiredCol.reset();
        return;
    }

    // Find start of previous line.
    std::size_t prev_line_end = line_start - 1; // the '\n' before current line
    std::size_t prev_line_start = prev_line_end;
    while (prev_line_start > 0 && _data[prev_line_start - 1] != '\n')
        --prev_line_start;

    // Walk the previous line to find the position matching the goal visual column.
    std::size_t pos = prev_line_start;
    std::size_t v = 0;
    while (pos < prev_line_end) {
        auto info = charInfoAt(_data.data() + pos, prev_line_end - pos);
        if (v + info.width > goal) break;
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
    std::size_t line_start = _point;
    while (line_start > 0 && _data[line_start - 1] != '\n')
        --line_start;
    std::size_t vcol = visualCol(line_start, _point);

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
        if (v + info.width > goal) break;
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

void Buffer::scrollDown() {
    // Advance _viewFrom past the next newline.
    while (_viewFrom < _data.size() && _data[_viewFrom] != '\n')
        ++_viewFrom;
    if (_viewFrom < _data.size())
        ++_viewFrom; // skip the '\n'
}

void Buffer::gotoLine(std::size_t line, RowIdx screenRows) {
    // Move _point to the start of the 1-based line number.
    // If line exceeds the line count, move to the last line.
    if (line == 0) line = 1;

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

std::tuple<std::size_t, std::size_t> Buffer::lineCol() const {
    // Count code points (not bytes) for the column.
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

std::tuple<RowIdx, ColIdx> Buffer::render(std::string& out, RowIdx rows, ColIdx cols) const {
    static constexpr auto kCtrlColor = "\x1b[38;2;204;120;50m";
    static constexpr auto kTabStyle  = "\x1b[48;2;70;70;70m";
    static constexpr auto kReset     = "\x1b[0m";

    // Per-row output: the ANSI-escaped content and its visual width.
    struct RowData {
        std::string content;
        std::size_t width = 0;
    };
    std::vector<RowData> screen(rows.value);

    std::size_t row = 0, col = 0;
    std::size_t cur_row = 0, cur_col = 0;

    for (std::size_t i = _viewFrom; i < _data.size() && row < rows.value;) {
        char ch = _data[i];

        if (ch == '\n') {
            if (i == _point) { cur_row = row; cur_col = col; }
            ++row;
            col = 0;
            ++i;
            continue;
        }

        auto u = static_cast<unsigned char>(ch);
        auto info = charInfoAt(_data.data() + i, _data.size() - i);

        // Wrap if the representation doesn't fit on the current line.
        if (col + info.width > cols.value) {
            ++row;
            col = 0;
            if (row >= rows.value) break;
        }

        if (i == _point) { cur_row = row; cur_col = col; }

        auto& rd = screen[row];
        if (u < 0x80) {
            // ASCII: handle control characters specially.
            if (ch == '\t') {
                rd.content += kTabStyle;
                rd.content += "  ";
                rd.content += kReset;
            } else if (u == 0) {
                rd.content += kCtrlColor;
                rd.content += "\\0";
                rd.content += kReset;
            } else if (u == 0x7f) {
                rd.content += kCtrlColor;
                rd.content += "^?";
                rd.content += kReset;
            } else if (u < 0x20) {
                rd.content += kCtrlColor;
                rd.content += '^';
                rd.content += static_cast<char>('@' + u);
                rd.content += kReset;
            } else {
                rd.content += ch;
            }
        } else {
            // Multi-byte UTF-8: emit the whole sequence.
            rd.content.append(_data.data() + i, info.len);
        }
        rd.width += info.width;
        col += info.width;

        if (col >= cols.value) {
            ++row;
            col = 0;
        }

        i += info.len;
    }

    // If point is at or past the end of data, its position is wherever
    // the scan ended up.
    if (_point >= _data.size()) {
        cur_row = row;
        cur_col = col;
    }

    // Append each row with explicit cursor positioning, padded with spaces.
    for (std::size_t r = 0; r < rows.value; ++r) {
        out += "\x1b[";
        out += std::to_string(r + 1);
        out += ";1H";
        out += screen[r].content;
        if (screen[r].width < cols.value)
            out.append(cols.value - screen[r].width, ' ');
    }

    return {RowIdx{cur_row}, ColIdx{cur_col}};
}
