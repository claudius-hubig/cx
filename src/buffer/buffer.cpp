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

void Buffer::overwrite(char c) {
    if (_point >= _data.size())
        _data.push_back(c);
    else
        _data[_point] = c;
}

void Buffer::render(std::string& out, BufferPoint from, RowIdx rows, ColIdx cols) const {
    // Build a row-major screen image, then write each row with an explicit
    // ANSI cursor position. Also track where the point falls on screen.
    std::vector<char> screen(rows.value * cols.value, ' ');
    std::size_t row = 0, col = 0;
    std::size_t cur_row = 0, cur_col = 0;

    for (std::size_t i = from; i < _data.size() && row < rows.value; ++i) {
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

    // Reposition the terminal cursor to match the buffer point.
    out += "\x1b[";
    out += std::to_string(cur_row + 1);
    out += ";";
    out += std::to_string(cur_col + 1);
    out += "H";
}
