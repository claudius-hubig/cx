#include "screen.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <string>
#include <variant>

// Set by the SIGWINCH handler; checked in loop() to trigger a resize.
static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int) {
    g_resized = 1;
}

Screen::Screen() : _size{RowIdx{0}, ColIdx{0}}, _cursor{RowIdx{0}, ColIdx{0}} {
    // TIOCGWINSZ is the standard POSIX ioctl for querying the terminal
    // dimensions in rows and columns.
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};

    // Initialise with spaces rather than null bytes: the buffer is written
    // directly to the terminal as a flat byte stream, so every cell must
    // occupy exactly one column of screen space.
    auto& [rows, cols] = _size;
    _buffer.assign(rows.value * cols.value, ' ');

    // Use sigaction() instead of signal() to avoid SA_RESTART, which glibc
    // sets by default. Without SA_RESTART, read() returns EINTR on SIGWINCH
    // so the loop can detect the resize promptly.
    struct sigaction sa = {};
    sa.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &sa, nullptr);
}

void Screen::draw() {
    auto& [rows, cols] = _size;
    auto& [cur_row, cur_col] = _cursor;

    // Write each row with an explicit cursor position rather than relying on
    // the terminal wrapping the flat buffer at the right column boundary.
    // After a resize the terminal may have already reformatted its display,
    // so implicit wrapping would mis-align subsequent rows.
    // Build the full output in one string and flush it in a single write().
    std::string out;
    out.reserve(rows.value * cols.value + rows.value * 8);

    for (std::size_t r = 0; r < rows.value; ++r) {
        out += "\x1b[";
        out += std::to_string(r + 1);
        out += ";1H";
        out.append(_buffer.data() + r * cols.value, cols.value);
    }

    // Reposition the terminal cursor to match the logical cursor.
    out += "\x1b[";
    out += std::to_string(cur_row.value + 1);
    out += ";";
    out += std::to_string(cur_col.value + 1);
    out += "H";

    write(STDOUT_FILENO, out.data(), out.size());
}

void Screen::loop() {
    char c;
    while (true) {
        // read() returns -1 with errno == EINTR when interrupted by a signal
        // (e.g. SIGWINCH). In that case check the resize flag and retry.
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == -1 && errno == EINTR) {
            if (g_resized) {
                g_resized = 0;
                resize();
                draw();
            }
            continue;
        }
        if (n != 1) break;

        // Also check after a successful read, in case the signal arrived
        // between loop iterations rather than during a blocked read().
        if (g_resized) {
            g_resized = 0;
            resize();
            draw();
        }

        auto& [row, col] = _cursor;

        // In raw mode the terminal does not translate CR to NL, but some
        // terminal emulators send \n for Enter anyway — handle both.
        auto& [rows, cols] = _size;
        if (c == '\r' || c == '\n') {
            if (row.value < rows.value - 1) {
                ++row.value;
                col.value = 0;
            }
        } else if (c < 0x20 || c == 0x7f) {
            // Control character: delegate to handleSpecialInput().
            auto result = handleSpecialInput(c);
            if (std::holds_alternative<Action>(result) &&
                std::get<Action>(result) == Action::Exit)
                break;
            if (std::holds_alternative<std::string>(result)) {
                for (char ch : std::get<std::string>(result)) {
                    set(row, col, ch);
                    if (col.value < cols.value - 1) {
                        ++col.value;
                    } else if (row.value < rows.value - 1) {
                        col.value = 0;
                        ++row.value;
                    } else {
                        break; // bottom-right corner: stop
                    }
                }
            }
        } else {
            set(row, col, c);

            bool at_last_col = col.value == cols.value - 1;
            bool at_last_row = row.value == rows.value - 1;

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

void Screen::resize() {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    auto new_size = std::tuple{RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
    auto& [new_rows, new_cols] = new_size;
    auto& [old_rows, old_cols] = _size;

    // Allocate the new buffer filled with spaces, then copy the top-left
    // corner that fits in both the old and new dimensions.
    std::vector<char> new_buffer(new_rows.value * new_cols.value, ' ');
    std::size_t copy_rows = std::min(old_rows.value, new_rows.value);
    std::size_t copy_cols = std::min(old_cols.value, new_cols.value);
    for (std::size_t r = 0; r < copy_rows; ++r)
        for (std::size_t c = 0; c < copy_cols; ++c)
            new_buffer[r * new_cols.value + c] = _buffer[r * old_cols.value + c];

    _buffer = std::move(new_buffer);
    _size = new_size;

    // Clamp cursor to the new bounds.
    auto& [row, col] = _cursor;
    if (row.value >= new_rows.value) row.value = new_rows.value - 1;
    if (col.value >= new_cols.value) col.value = new_cols.value - 1;
}

std::variant<std::monostate, std::string, Action> Screen::handleSpecialInput(char c) {
    switch (c) {
        case 0x03: return Action::Exit;   // Ctrl-C
        case 0x18: return std::string{"hello"};  // Ctrl-X
        default:   return std::monostate{};
    }
}

void Screen::set(RowIdx row, ColIdx col, char value) {
    auto& [rows, cols] = _size;
    _buffer[static_cast<std::size_t>(row) * cols.value + static_cast<std::size_t>(col)] = value;
}
