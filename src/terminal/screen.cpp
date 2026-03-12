#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_manager.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <string>
#include <variant>

// Set by the SIGWINCH handler; checked in loop() to trigger a resize.
static volatile sig_atomic_t g_resized = 0;

static void handle_sigwinch(int) {
    g_resized = 1;
}

Screen::Screen() : _size{RowIdx{0}, ColIdx{0}} {
    // TIOCGWINSZ is the standard POSIX ioctl for querying the terminal
    // dimensions in rows and columns.
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};

    // Use sigaction() instead of signal() to avoid SA_RESTART, which glibc
    // sets by default. Without SA_RESTART, read() returns EINTR on SIGWINCH
    // so the loop can detect the resize promptly.
    struct sigaction sa = {};
    sa.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &sa, nullptr);

    draw();
}

void Screen::setBuffer(Buffer& buf) {
    _buf = &buf;
    draw();
}

void Screen::setBufferManager(BufferManager& mgr) {
    _bufMgr = &mgr;
}

void Screen::draw() {
    auto& [rows, cols] = _size;

    // Reserve the last row for the status line.
    RowIdx buf_rows{rows.value > 1 ? rows.value - 1 : 1};

    _out.clear();

    RowIdx cur_row{0};
    ColIdx cur_col{0};
    if (_buf)
        std::tie(cur_row, cur_col) = _buf->render(_out, buf_rows, cols);

    // Status line on the last row, with inverted colours.
    std::string status;
    if (_prompt == Prompt::OpenFile)
        status = "File to open: " + _prompt_input;
    else if (_prompt == Prompt::GoToLine)
        status = "Go to line: " + _prompt_input;
    else if (_buf) {
        auto [line, col] = _buf->lineCol();
        status = std::to_string(line) + ":" + std::to_string(col);
    }

    // Right-align the buffer name on the status line.
    std::string buf_name;
    if (_buf)
        buf_name = _buf->name();
    if (status.size() + buf_name.size() + 2 <= cols.value) {
        status.resize(cols.value - buf_name.size(), ' ');
        status += buf_name;
    } else {
        status.resize(cols.value, ' ');
    }
    _out += "\x1b[";
    _out += std::to_string(rows.value);
    _out += ";1H";
    _out += "\x1b[7m"; // reverse video
    _out += status;
    _out += "\x1b[0m"; // reset

    // Position the terminal cursor AFTER the status line, so it lands
    // in the buffer area rather than on the status bar.
    _out += "\x1b[";
    _out += std::to_string(cur_row.value + 1);
    _out += ";";
    _out += std::to_string(cur_col.value + 1);
    _out += "H";

    // Ensure the cursor is visible.
    _out += "\x1b[?25h";

    write(STDOUT_FILENO, _out.data(), _out.size());
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

        // Prompt mode: keystrokes build the prompt input instead of editing
        // the buffer. Enter confirms, Escape (0x1b) cancels.
        if (_prompt != Prompt::None) {
            if (c == '\r' || c == '\n') {
                if (!_prompt_input.empty()) {
                    if (_prompt == Prompt::OpenFile && _bufMgr) {
                        auto& buf = _bufMgr->open(_prompt_input);
                        setBuffer(buf);
                    } else if (_prompt == Prompt::GoToLine && _buf) {
                        auto& [rows, cols] = _size;
                        RowIdx buf_rows{rows.value > 1 ? rows.value - 1 : 1};
                        std::size_t line = 0;
                        if (_prompt_input == "last")
                            line = SIZE_MAX;
                        else {
                            for (char ch : _prompt_input) {
                                if (ch >= '0' && ch <= '9')
                                    line = line * 10 + (ch - '0');
                            }
                        }
                        if (line > 0)
                            _buf->gotoLine(line, buf_rows);
                    }
                }
                _prompt = Prompt::None;
                _prompt_input.clear();
            } else if (c == 0x1b) {
                _prompt = Prompt::None;
                _prompt_input.clear();
            } else if (c == 0x7f && !_prompt_input.empty()) {
                _prompt_input.pop_back();
            } else if (c >= 0x20) {
                _prompt_input += c;
            }
        } else if (c == '\r' || c == '\n') {
            // In raw mode the terminal does not translate CR to NL, but some
            // terminal emulators send \n for Enter anyway — handle both.
            if (_buf) {
                _buf->insert('\n');
                auto& [rows, cols] = _size;
                RowIdx buf_rows{rows.value > 1 ? rows.value - 1 : 1};
                while (_buf->screenRow(cols) >= buf_rows.value)
                    _buf->scrollDown();
            }
        } else if (c == 0x7f) {
            // Backspace: delete character before point.
            if (_buf) _buf->deleteBeforePoint();
        } else if (c == 0x1b) {
            // Use poll() to distinguish bare Esc from escape sequences.
            struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
            if (poll(&pfd, 1, 50) > 0) {
                // Escape sequence: read the next bytes to identify the key.
                char seq[2];
                if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                    if (read(STDIN_FILENO, &seq[1], 1) == 1) {
                        switch (seq[1]) {
                            case 'A':
                                if (_buf) {
                                    _buf->moveUp();
                                    if (_buf->point() < _buf->viewFrom())
                                        _buf->scrollUp();
                                }
                                break;
                            case 'B':
                                if (_buf) {
                                    _buf->moveDown();
                                    auto& [rows, cols] = _size;
                                    RowIdx buf_rows{rows.value > 1 ? rows.value - 1 : 1};
                                    if (_buf->screenRow(cols) >= buf_rows.value)
                                        _buf->scrollDown();
                                }
                                break;
                            case 'C': if (_buf) _buf->moveRight(); break;
                            case 'D': if (_buf) _buf->moveLeft(); break;
                            case 'H': if (_buf) _buf->moveHome(); break;
                            case 'F': if (_buf) _buf->moveEnd(); break;
                            case '3': {
                                char tilde;
                                if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~')
                                    if (_buf) _buf->deleteAtPoint();
                                break;
                            }
                        }
                    }
                }
            } else if (_buf && _buf->readonly() && _prevBuf) {
                // Bare Esc in help buffer: switch back.
                setBuffer(*_prevBuf);
                _prevBuf = nullptr;
            }
        } else if (c < 0x20) {
            // Control character: delegate to handleSpecialInput().
            auto result = handleSpecialInput(c);
            if (std::holds_alternative<Action>(result)) {
                auto action = std::get<Action>(result);
                if (action == Action::Exit)
                    break;
                if (action == Action::OpenFile) {
                    _prompt = Prompt::OpenFile;
                    _prompt_input.clear();
                }
                if (action == Action::GoToLine) {
                    _prompt = Prompt::GoToLine;
                    _prompt_input.clear();
                }
                if (action == Action::Help && _bufMgr) {
                    _prevBuf = _buf;
                    setBuffer(_bufMgr->helpBuffer());
                }
            }
            if (std::holds_alternative<std::string>(result) && _buf) {
                for (char ch : std::get<std::string>(result))
                    _buf->insert(ch);
            }
        } else {
            if (_buf && _buf->readonly() && c == 'q' && _prevBuf) {
                setBuffer(*_prevBuf);
                _prevBuf = nullptr;
            } else if (_buf) {
                _buf->insert(c);
            }
        }

        draw();
    }
}

void Screen::resize() {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
    // No buffer to resize — render() produces a fresh screen image each time.
    // No cursor to clamp — it's derived from the buffer point.
}

std::variant<std::monostate, std::string, Action> Screen::handleSpecialInput(char c) {
    switch (c) {
        case 0x03: return Action::Exit;   // Ctrl-C
        case 0x08: return Action::Help;    // Ctrl-H
        case 0x0c: return Action::GoToLine; // Ctrl-L
        case 0x0f: return Action::OpenFile; // Ctrl-O
        case 0x18: return std::string{"hello"};  // Ctrl-X
        default:   return std::monostate{};
    }
}
