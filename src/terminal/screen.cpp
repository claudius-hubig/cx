#include "screen.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <string>

Screen::Screen() : _cursor{RowIdx{0}, ColIdx{0}} {
    // TIOCGWINSZ is the standard POSIX ioctl for querying the terminal
    // dimensions in rows and columns.
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    _rows = ws.ws_row;
    _cols = ws.ws_col;

    // Initialise with spaces rather than null bytes: the buffer is written
    // directly to the terminal as a flat byte stream, so every cell must
    // occupy exactly one column of screen space.
    _buffer.assign(_rows * _cols, ' ');
}

void Screen::draw() {
    auto& [row, col] = _cursor;

    // Move cursor to top-left, then write the entire buffer in one call.
    write(STDOUT_FILENO, "\x1b[H", 3);
    write(STDOUT_FILENO, _buffer.data(), _buffer.size());

    // Reposition the terminal cursor to match the logical cursor.
    std::string seq = "\x1b[" + std::to_string(row.value + 1) + ";" + std::to_string(col.value + 1) + "H";
    write(STDOUT_FILENO, seq.data(), seq.size());
}

void Screen::loop() {
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        auto& [row, col] = _cursor;

        // In raw mode the terminal does not translate CR to NL, but some
        // terminal emulators send \n for Enter anyway — handle both.
        if (c == '\r' || c == '\n') {
            if (row.value < _rows - 1) {
                ++row.value;
                col.value = 0;
            }
        } else {
            set(row, col, c);

            bool at_last_col = col.value == _cols - 1;
            bool at_last_row = row.value == _rows - 1;

            if (!at_last_col)
                ++col.value;
            else if (!at_last_row) {
                col.value = 0;
                ++row.value;
            }
            // bottom-right corner: cursor stays, next char overwrites in place
        }

        draw();
    }
}

void Screen::set(RowIdx row, ColIdx col, char value) {
    _buffer[static_cast<std::size_t>(row) * _cols + static_cast<std::size_t>(col)] = value;
}
