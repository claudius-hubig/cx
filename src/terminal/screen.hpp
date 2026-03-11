#pragma once

#include <cstddef>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

enum class Action {
    Exit,
};

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
    // redraw after each keystroke. Returns when stdin is closed or on Action::Exit.
    void loop();

    // Interpret a control character and return the appropriate response:
    // Action::Exit on Ctrl-C, a string to insert on Ctrl-X, or monostate
    // for unhandled inputs.
    std::variant<std::monostate, std::string, Action> handleSpecialInput(char c);

private:
    // Query the new terminal size, allocate a fresh buffer initialised to
    // spaces, copy the top-left overlap from the old buffer, and clamp the
    // cursor to the new bounds.
    void resize();
    std::tuple<RowIdx, ColIdx> _size;
    std::vector<char> _buffer;
    std::tuple<RowIdx, ColIdx> _cursor;
};
