#pragma once

#include <cstddef>
#include <tuple>
#include <vector>

struct RowIdx {
    explicit RowIdx(std::size_t value) : value(value) {}
    operator std::size_t() const { return value; }
    std::size_t value;
};

struct ColIdx {
    explicit ColIdx(std::size_t value) : value(value) {}
    operator std::size_t() const { return value; }
    std::size_t value;
};

// Manages the terminal screen buffer and the main input loop.
//
// On construction, queries the terminal size and allocates a flat
// row-major character buffer initialised to spaces. draw() renders the
// full buffer to the terminal and repositions the hardware cursor.
// loop() reads raw keystrokes and updates the buffer and cursor accordingly.
class Screen {
public:
    Screen();

    // Write character value into the buffer at (row, col).
    void set(RowIdx row, ColIdx col, char value);

    // Render the entire buffer to the terminal and move the hardware
    // cursor to the current logical cursor position.
    void draw();

    // Read keystrokes one character at a time, update the buffer, and
    // redraw after each keystroke. Returns when stdin is closed.
    void loop();

private:
    std::size_t _rows;
    std::size_t _cols;
    std::vector<char> _buffer;
    std::tuple<RowIdx, ColIdx> _cursor;
};
