#include "buffer.hpp"

#include <fstream>
#include <iterator>

Buffer::Buffer(std::string name) : _name(std::move(name)) {}

Buffer::Buffer(std::string name, std::filesystem::path const& path)
    : _name(std::move(name)), _path(path) {
    std::ifstream file(path, std::ios::binary);
    if (file)
        _data.assign(std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>());
}

void Buffer::insert(char c) {
    if (_point >= _data.size())
        _data.push_back(c);
    else
        _data.insert(_data.begin() + _point, c);
    ++_point;
}

void Buffer::deleteAtPoint() {
    if (_point < _data.size())
        _data.erase(_data.begin() + _point);
}

void Buffer::deleteBeforePoint() {
    if (_point > 0) {
        --_point;
        _data.erase(_data.begin() + _point);
    }
}

void Buffer::moveLeft() {
    if (_point > 0)
        --_point;
}

void Buffer::moveRight() {
    if (_point < _data.size())
        ++_point;
}

void Buffer::moveUp() {
    // Find start of current line and compute column.
    std::size_t line_start = _point;
    while (line_start > 0 && _data[line_start - 1] != '\n')
        --line_start;
    std::size_t col = _point - line_start;

    // If already on the first line, move to beginning.
    if (line_start == 0) {
        _point = 0;
        return;
    }

    // Find start of previous line.
    std::size_t prev_line_end = line_start - 1; // the '\n' before current line
    std::size_t prev_line_start = prev_line_end;
    while (prev_line_start > 0 && _data[prev_line_start - 1] != '\n')
        --prev_line_start;

    std::size_t prev_line_len = prev_line_end - prev_line_start;
    _point = prev_line_start + std::min(col, prev_line_len);
}

void Buffer::moveDown() {
    // Find start of current line and compute column.
    std::size_t line_start = _point;
    while (line_start > 0 && _data[line_start - 1] != '\n')
        --line_start;
    std::size_t col = _point - line_start;

    // Find start of next line (past the '\n' after current position).
    std::size_t next_line_start = _point;
    while (next_line_start < _data.size() && _data[next_line_start] != '\n')
        ++next_line_start;

    // If no newline found, move to end.
    if (next_line_start >= _data.size()) {
        _point = _data.size();
        return;
    }

    ++next_line_start; // skip the '\n'

    // Find length of next line.
    std::size_t next_line_end = next_line_start;
    while (next_line_end < _data.size() && _data[next_line_end] != '\n')
        ++next_line_end;

    std::size_t next_line_len = next_line_end - next_line_start;
    _point = next_line_start + std::min(col, next_line_len);
}

void Buffer::scrollDown() {
    // Advance _viewFrom past the next newline.
    while (_viewFrom < _data.size() && _data[_viewFrom] != '\n')
        ++_viewFrom;
    if (_viewFrom < _data.size())
        ++_viewFrom; // skip the '\n'
}

std::size_t Buffer::screenRow(ColIdx cols) const {
    std::size_t row = 0, col = 0;
    for (std::size_t i = _viewFrom; i < _point && i < _data.size(); ++i) {
        if (_data[i] == '\n') {
            ++row;
            col = 0;
        } else {
            ++col;
            if (col >= cols.value) {
                col = 0;
                ++row;
            }
        }
    }
    return row;
}

void Buffer::overwrite(char c) {
    if (_point >= _data.size())
        _data.push_back(c);
    else
        _data[_point] = c;
}

std::tuple<RowIdx, ColIdx> Buffer::render(std::string& out, RowIdx rows, ColIdx cols) const {
    // Build a row-major screen image, then write each row with an explicit
    // ANSI cursor position. Also track where the point falls on screen.
    std::vector<char> screen(rows.value * cols.value, ' ');
    std::size_t row = 0, col = 0;
    std::size_t cur_row = 0, cur_col = 0;

    for (std::size_t i = _viewFrom; i < _data.size() && row < rows.value; ++i) {
        if (i == _point) {
            cur_row = row;
            cur_col = col;
        }
        if (_data[i] == '\n') {
            ++row;
            col = 0;
        } else {
            screen[row * cols.value + col] = _data[i];
            ++col;
            if (col >= cols.value) {
                col = 0;
                ++row;
            }
        }
    }

    // If point is at or past the end of data, its position is wherever
    // the scan ended up.
    if (_point >= _data.size()) {
        cur_row = row;
        cur_col = col;
    }

    // Append each row with explicit cursor positioning.
    for (std::size_t r = 0; r < rows.value; ++r) {
        out += "\x1b[";
        out += std::to_string(r + 1);
        out += ";1H";
        out.append(screen.data() + r * cols.value, cols.value);
    }

    return {RowIdx{cur_row}, ColIdx{cur_col}};
}
