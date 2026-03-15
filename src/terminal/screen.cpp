#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_manager.hpp"
#include "buffer/char_width.hpp"
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <limits>
#include <string>
#include <variant>

// Read remaining bytes of a UTF-8 sequence from stdin, given the lead byte.
// Appends continuation bytes to `dest`.
static void readUtf8Tail(unsigned char lead, std::string& dest) {
    for (std::size_t j = 1; j < utf8SeqLen(lead); ++j) {
        char next;
        if (read(STDIN_FILENO, &next, 1) != 1) break;
        dest += next;
    }
}

// Insert a character (and any UTF-8 continuation bytes) into a buffer.
static void insertCharWithUtf8(Buffer& buf, char c) {
    buf.insert(c);
    auto u = static_cast<unsigned char>(c);
    if (u >= 0x80) {
        for (std::size_t j = 1; j < utf8SeqLen(u); ++j) {
            char next;
            if (read(STDIN_FILENO, &next, 1) != 1) break;
            buf.insert(next);
        }
    }
}

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
    if (_bufMgr && !buf.readonly())
        _bufMgr->touch(buf.name());
    draw();
}

void Screen::setBufferManager(BufferManager& mgr) {
    _bufMgr = &mgr;
}

void Screen::draw() {
    auto& [rows, cols] = _size;
    auto br = bufRows();

    _out.clear();

    RowIdx cur_row{0};
    ColIdx cur_col{0};
    if (_buf)
        std::tie(cur_row, cur_col) = _buf->render(_out, br, cols, _showWhitespace);

    // Expire timed status message.
    if (_statusExpiry && std::chrono::steady_clock::now() >= *_statusExpiry) {
        _statusMsg.clear();
        _statusExpiry.reset();
    }

    // Status line on the last row, with inverted colours.
    std::string status;
    std::size_t status_vwidth = 0; // visual width (excludes ANSI escapes)
    if (_prompt == Prompt::GoToLine) {
        status = "Go to line: " + _prompt_input;
        status_vwidth = status.size();
    } else if (_prompt == Prompt::SaveAs) {
        status = "Save as: " + _prompt_input;
        status_vwidth = status.size();
    } else if (_prompt == Prompt::SwitchBuffer) {
        status = "Buffer: " + _prompt_input;
        status_vwidth = status.size();
        // Append completion hints (dim text). A completion's match
        // string may or may not share the typed prefix (e.g. filename
        // matches against a full-path match string).
        auto hintFor = [&](std::string const& match) -> std::string {
            if (match.starts_with(_prompt_input))
                return match.substr(_prompt_input.size());
            return match;
        };
        if (_completionIdx < 0 && !_completions.empty()) {
            auto hint = hintFor(_completions[0].first);
            if (_completions.size() == 1) {
                if (!hint.empty()) {
                    status += "\x1b[2m" + hint + "\x1b[22m";
                    status_vwidth += hint.size();
                }
            } else if (_completions.size() <= 3) {
                status += "\x1b[2m[";
                status_vwidth++;
                for (std::size_t i = 0; i < _completions.size(); ++i) {
                    if (i > 0) { status += "/"; status_vwidth++; }
                    auto h = hintFor(_completions[i].first);
                    if (h.empty()) h = _completions[i].first;
                    status += h;
                    status_vwidth += h.size();
                }
                status += "]\x1b[22m";
                status_vwidth++;
            } else {
                auto cnt = " (" + std::to_string(_completions.size())
                           + " matches)";
                status += "\x1b[2m" + hint + cnt + "\x1b[22m";
                status_vwidth += hint.size() + cnt.size();
            }
        }
    } else if (_prompt == Prompt::ConfirmSave || _prompt == Prompt::ConfirmKill) {
        status = _prompt_input;
        status_vwidth = status.size();
    } else if (_buf) {
        auto [line, col] = _buf->lineCol();
        status = std::to_string(line) + ":" + std::to_string(col);
        if (!_statusMsg.empty())
            status += " | " + _statusMsg;
        status_vwidth = status.size();
    }

    // Right-align the buffer name + modification indicator on the status line.
    std::string buf_name;
    if (_buf) {
        buf_name = _bufMgr ? _bufMgr->displayName(_buf->name()) : _buf->name();
        if (_buf->modified())
            buf_name += " +";
        else
            buf_name += "  ";
    }
    if (status_vwidth + buf_name.size() + 2 <= cols.value) {
        status.append(cols.value - buf_name.size() - status_vwidth, ' ');
        status += buf_name;
    } else if (status_vwidth < cols.value) {
        status.append(cols.value - status_vwidth, ' ');
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
        if (_prompt == Prompt::ConfirmSave) {
            if (c == 'y' || c == 'Y') {
                if (_buf && _buf->save()) {
                    setStatus("Saved to " + _buf->name());
                    if (_killPending && _bufMgr) {
                        _killPending = false;
                        auto name = _buf->name();
                        _prompt = Prompt::None;
                        _prompt_input.clear();
                        if (_prevBuf == _buf) _prevBuf = nullptr;
                        setBuffer(_bufMgr->kill(name));
                        draw();
                        continue;
                    }
                } else if (_buf) {
                    setStatus("Error saving to " + _buf->name());
                    _killPending = false;
                }
                _prompt = Prompt::None;
                _prompt_input.clear();
            } else if (c == 'n' || c == 'N' || c == 0x1b) {
                _prompt = Prompt::None;
                _prompt_input.clear();
                _killPending = false;
            }
            // Ignore all other keys.
        } else if (_prompt == Prompt::ConfirmKill) {
            if (c == 'y' || c == 'Y') {
                if (_buf && _bufMgr) {
                    if (_buf->hasPath()) {
                        // Modified file-backed: save then kill.
                        _buf->save();
                        auto name = _buf->name();
                        _prompt = Prompt::None;
                        _prompt_input.clear();
                        if (_prevBuf == _buf) _prevBuf = nullptr;
                        setBuffer(_bufMgr->kill(name));
                    } else {
                        // Temp buffer: enter SaveAs, then kill after save.
                        _killPending = true;
                        _prompt = Prompt::SaveAs;
                        _prompt_input.clear();
                    }
                }
            } else if (c == 'n' || c == 'N') {
                // Kill without saving.
                if (_buf && _bufMgr) {
                    auto name = _buf->name();
                    _prompt = Prompt::None;
                    _prompt_input.clear();
                    if (_prevBuf == _buf) _prevBuf = nullptr;
                    setBuffer(_bufMgr->kill(name));
                }
            } else if (c == 0x1b) {
                // Cancel.
                _prompt = Prompt::None;
                _prompt_input.clear();
            }
        } else if (_prompt != Prompt::None) {
            if (c == '\r' || c == '\n') {
                if (!_prompt_input.empty()) {
                    if (_prompt == Prompt::SwitchBuffer && _bufMgr) {
                        // Resolve target from completion or typed input.
                        std::string resolved;
                        if (_completionIdx >= 0
                            && _completionIdx < static_cast<int>(_completions.size()))
                            resolved = _completions[_completionIdx].second;
                        else if (_completions.size() == 1)
                            resolved = _completions[0].second;
                        Buffer* target = nullptr;
                        if (!resolved.empty())
                            target = _bufMgr->find(resolved);
                        if (!target)
                            target = _bufMgr->find(_prompt_input);
                        if (target) {
                            setBuffer(*target);
                        } else if (!_prompt_input.empty() && _prompt_input[0] == '*') {
                            std::string name = _prompt_input;
                            if (name.back() != '*')
                                name += '*';
                            target = _bufMgr->find(name);
                            if (target)
                                setBuffer(*target);
                            else
                                setBuffer(_bufMgr->create(name));
                        } else {
                            // Not an existing buffer — try opening as a file.
                            std::error_code fec;
                            if (std::filesystem::is_directory(_prompt_input, fec)) {
                                if (!_prompt_input.ends_with('/'))
                                    _prompt_input += '/';
                                updateCompletions();
                                draw();
                                continue;
                            }
                            auto& buf = _bufMgr->open(_prompt_input);
                            setBuffer(buf);
                        }
                    } else if (_prompt == Prompt::SaveAs && _buf && _bufMgr) {
                        auto targetExists = std::filesystem::exists(_prompt_input);
                        _bufMgr->rename(_buf->name(), _prompt_input);
                        if (targetExists) {
                            _prompt = Prompt::ConfirmSave;
                            _prompt_input = "Overwrite " + _buf->name()
                                + "? y/n: ";
                            draw();
                            continue;
                        }
                        if (_buf->save()) {
                            setStatus("Saved to " + _buf->name());
                            if (_killPending) {
                                _killPending = false;
                                auto name = _buf->name();
                                _prompt = Prompt::None;
                                _prompt_input.clear();
                                _completions.clear();
                                _completionIdx = -1;
                                if (_prevBuf == _buf) _prevBuf = nullptr;
                                setBuffer(_bufMgr->kill(name));
                                draw();
                                continue;
                            }
                        } else {
                            setStatus("Error saving to " + _buf->name());
                            _killPending = false;
                        }
                    } else if (_prompt == Prompt::GoToLine && _buf) {
                        std::size_t line = 0;
                        if (_prompt_input == "last")
                            line = std::numeric_limits<std::size_t>::max();
                        else {
                            for (char ch : _prompt_input) {
                                if (ch < '0' || ch > '9') { line = 0; break; }
                                auto digit = static_cast<std::size_t>(ch - '0');
                                if (line > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
                                    line = std::numeric_limits<std::size_t>::max();
                                    break;
                                }
                                line = line * 10 + digit;
                            }
                        }
                        if (line > 0)
                            _buf->gotoLine(line, bufRows());
                    }
                }
                _prompt = Prompt::None;
                _prompt_input.clear();
                _completions.clear();
                _completionIdx = -1;
            } else if (c == 0x1b) {
                // Distinguish bare Esc (cancel) from escape sequences.
                struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
                if (poll(&pfd, 1, 50) > 0) {
                    char seq[2];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '['
                        && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[1] == 'C' && _prompt == Prompt::SwitchBuffer
                            && !_completions.empty()) {
                            // Right arrow: accept the current completion.
                            if (_completionIdx >= 0)
                                _prompt_input = _completions[_completionIdx].first;
                            else if (_completions.size() == 1)
                                _prompt_input = _completions[0].first;
                            updateCompletions();
                        }
                        // Ignore other escape sequences in prompt mode.
                    }
                } else {
                    // Bare Esc: cancel prompt.
                    _prompt = Prompt::None;
                    _prompt_input.clear();
                    _completions.clear();
                    _completionIdx = -1;
                    _killPending = false;
                }
            } else if (c == '\t' && _prompt == Prompt::SwitchBuffer
                       && !_completions.empty()) {
                if (_completionIdx < 0)
                    _completionIdx = 0;
                else
                    _completionIdx = (_completionIdx + 1)
                                     % static_cast<int>(_completions.size());
                _prompt_input = _completions[_completionIdx].first;
                // Directory: navigate into it only when unambiguous.
                if (_prompt_input.ends_with('/') && _completions.size() == 1)
                    updateCompletions();
            } else if (c == 0x7f && !_prompt_input.empty()) {
                _prompt_input.pop_back();
                if (_prompt == Prompt::SwitchBuffer)
                    updateCompletions();
            } else if (static_cast<unsigned char>(c) >= 0x20) {
                _prompt_input += c;
                auto u = static_cast<unsigned char>(c);
                if (u >= 0x80) readUtf8Tail(u, _prompt_input);
                if (_prompt == Prompt::SwitchBuffer)
                    updateCompletions();
            }
        } else if (c == '\r' || c == '\n') {
            // Buffer list: Enter selects the highlighted buffer.
            auto* blb = dynamic_cast<BufferListBuffer*>(_buf);
            if (blb && _prevBuf && _bufMgr) {
                auto name = blb->selectedName();
                Buffer* target = _bufMgr->find(name);
                if (target) {
                    _prevBuf = nullptr;
                    setBuffer(*target);
                }
            } else if (_buf) {
                _buf->insert('\n');
                auto& [rows, cols] = _size;
                while (_buf->screenRow(cols) >= bufRows().value)
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
                                    while (_buf->screenRow(cols) >= bufRows().value)
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
                            default: {
                                // Drain remaining bytes of unrecognised escape sequence.
                                char discard;
                                struct pollfd dp = {STDIN_FILENO, POLLIN, 0};
                                while (poll(&dp, 1, 0) > 0)
                                    if (read(STDIN_FILENO, &discard, 1) != 1) break;
                                break;
                            }
                        }
                    }
                } else if (seq[0] != '[') {
                    // Not a CSI sequence — insert the byte verbatim
                    // (e.g. Alt+key sends ESC followed by the key).
                    if (_buf && static_cast<unsigned char>(seq[0]) >= 0x20)
                        insertCharWithUtf8(*_buf, seq[0]);
                }
            } else if (_buf && _buf->readonly() && _prevBuf) {
                // Bare Esc in help buffer: switch back.
                setBuffer(*_prevBuf);
                _prevBuf = nullptr;
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            // Control character: delegate to handleSpecialInput().
            auto result = handleSpecialInput(c);
            if (std::holds_alternative<Action>(result)) {
                auto action = std::get<Action>(result);
                if (action == Action::Exit)
                    break;
                else if (action == Action::GoToLine) {
                    _prompt = Prompt::GoToLine;
                    _prompt_input.clear();
                } else if (action == Action::Help && _bufMgr) {
                    if (_buf && _buf->readonly() && _prevBuf) {
                        // Already in a readonly overlay (e.g. help); dismiss it.
                        setBuffer(*_prevBuf);
                        _prevBuf = nullptr;
                    } else {
                        _prevBuf = _buf;
                        setBuffer(_bufMgr->helpBuffer());
                    }
                } else if (action == Action::BufferList && _bufMgr) {
                    if (_buf && _buf->readonly() && _prevBuf) {
                        setBuffer(*_prevBuf);
                        _prevBuf = nullptr;
                    } else {
                        _prevBuf = _buf;
                        setBuffer(_bufMgr->bufferListBuffer());
                    }
                } else if (action == Action::SwitchBuffer) {
                    _prevBuf = nullptr;
                    _prompt = Prompt::SwitchBuffer;
                    _prompt_input.clear();
                    updateCompletions();
                } else if (action == Action::KillBuffer && _buf && _bufMgr
                           && !_buf->readonly()) {
                    if (!_buf->modified()) {
                        // Unmodified: kill immediately.
                        auto name = _buf->name();
                        if (_prevBuf == _buf) _prevBuf = nullptr;
                        setBuffer(_bufMgr->kill(name));
                    } else if (!_buf->hasPath()) {
                        _prompt = Prompt::ConfirmKill;
                        _prompt_input =
                            "Killing temporary buffer. Save to file first? y/n: ";
                    } else {
                        _prompt = Prompt::ConfirmKill;
                        _prompt_input =
                            "Killing modified buffer. Save to file first? y/n: ";
                    }
                } else if (action == Action::ToggleWhitespace) {
                    _showWhitespace = !_showWhitespace;
                } else if (action == Action::Save && _buf
                           && !_buf->readonly()) {
                    if (_buf->hasPath()) {
                        if (!std::filesystem::exists(_buf->name())) {
                            _prompt = Prompt::ConfirmSave;
                            _prompt_input = "File " + _buf->name()
                                + " doesn't exist. Really save there? y/n: ";
                        } else if (_buf->save()) {
                            setStatus("Saved to " + _buf->name());
                        } else {
                            setStatus("Error saving to " + _buf->name());
                        }
                    } else {
                        _prompt = Prompt::SaveAs;
                        _prompt_input.clear();
                    }
                }
            } else if (std::holds_alternative<std::string>(result) && _buf) {
                for (char ch : std::get<std::string>(result))
                    _buf->insert(ch);
            }
        } else {
            if (_buf && _buf->readonly() && c == 'q' && _prevBuf) {
                setBuffer(*_prevBuf);
                _prevBuf = nullptr;
            } else if (_buf) {
                insertCharWithUtf8(*_buf, c);
            }
        }

        draw();
    }
}

RowIdx Screen::bufRows() const {
    auto& [rows, cols] = _size;
    return RowIdx{rows.value > 1 ? rows.value - 1 : 1};
}

void Screen::resize() {
    struct winsize ws;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
    // No buffer to resize — render() produces a fresh screen image each time.
    // No cursor to clamp — it's derived from the buffer point.
}

void Screen::updateCompletions() {
    if (_prompt == Prompt::SwitchBuffer && _bufMgr) {
        _completions = _bufMgr->completions(_prompt_input);
        _completionIdx = -1;
    } else {
        _completions.clear();
        _completionIdx = -1;
    }
}

void Screen::setStatus(std::string msg) {
    _statusMsg = std::move(msg);
    _statusExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(10);
}

std::variant<std::monostate, std::string, Action> Screen::handleSpecialInput(char c) {
    switch (c) {
        case 0x02: return Action::SwitchBuffer; // Ctrl-B
        case 0x03: return Action::Exit;   // Ctrl-C
        case 0x08: return Action::Help;    // Ctrl-H
        case 0x0b: return Action::KillBuffer; // Ctrl-K
        case 0x0e: return Action::BufferList; // Ctrl-N
        case 0x0c: return Action::GoToLine; // Ctrl-L
        case 0x13: return Action::Save;    // Ctrl-S
        case 0x17: return Action::ToggleWhitespace; // Ctrl-W
        default:   return std::monostate{};
    }
}
