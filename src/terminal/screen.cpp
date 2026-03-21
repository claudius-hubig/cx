#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_manager.hpp"
#include "utf8.hpp"
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>

// Read remaining bytes of a UTF-8 sequence from stdin, given the lead byte.
// Appends continuation bytes to `dest`.
static void readUtf8Tail(unsigned char lead, std::string& dest) {
  for (std::size_t j = 1; j < utf8SeqLen(lead); ++j) {
    char next;
    if (read(STDIN_FILENO, &next, 1) != 1)
      break;
    dest += next;
  }
}

// Read a full UTF-8 character from stdin (given the lead byte) and insert
// it into the buffer as a single codepoint.
static void insertCharWithUtf8(Buffer& buf, char c) {
  auto u = static_cast<unsigned char>(c);
  if (u < 0x80) {
    buf.insert(static_cast<uint32_t>(u));
    return;
  }
  char seq[4];
  seq[0] = c;
  std::size_t expected = utf8SeqLen(u);
  std::size_t got = 1;
  for (std::size_t j = 1; j < expected; ++j) {
    char next;
    if (read(STDIN_FILENO, &next, 1) != 1)
      break;
    seq[got++] = next;
  }
  auto [cp, len] = decodeUtf8(seq, got);
  buf.insert(cp);
}

// Write all bytes to fd, retrying on partial writes and EINTR.
static void writeAll(int fd, char const* data, std::size_t len) {
  while (len > 0) {
    ssize_t n = write(fd, data, len);
    if (n >= 0) {
      data += n;
      len -= static_cast<std::size_t>(n);
    } else if (errno != EINTR) {
      break;
    }
  }
}

// Set by the SIGWINCH handler; checked in loop() to trigger a resize.
static sig_atomic_t volatile g_resized = 0;

static void handle_sigwinch(int) { g_resized = 1; }

Screen::Screen() : _size{RowIdx{0}, ColIdx{0}} {
  // TIOCGWINSZ is the standard POSIX ioctl for querying the terminal
  // dimensions in rows and columns.
  struct winsize ws = {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
  else
    _size = {RowIdx{24}, ColIdx{80}}; // sensible fallback

  // Use sigaction() instead of signal() to avoid SA_RESTART, which glibc
  // sets by default. Without SA_RESTART, read() returns EINTR on SIGWINCH
  // so the loop can detect the resize promptly.
  struct sigaction sa = {};
  sa.sa_handler = handle_sigwinch;
  sigaction(SIGWINCH, &sa, nullptr);

  // Set the terminal title.
  auto cwd = std::filesystem::current_path().string();
  std::string title = "\x1b]0;cx@" + cwd + "\x07";
  writeAll(STDOUT_FILENO, title.data(), title.size());

  draw();
}

void Screen::setBuffer(Buffer& buf) {
  _buf = &buf;
  _pasteState.reset();
  if (_bufMgr && !buf.readonly())
    _bufMgr->touch(buf.name());
  draw();
}

void Screen::setBufferManager(BufferManager& mgr) { _bufMgr = &mgr; }

void Screen::draw() {
  auto& [rows, cols] = _size;
  auto br = rowsForBuffer();

  _renderBuf.clear();

  RowIdx cur_row{0};
  ColIdx cur_col{0};
  if (_buf) {
    _buf->ensureVisible(br, cols, _wrap);
    std::tie(cur_row, cur_col) = _buf->render(_renderBuf, br, cols, _showWhitespace, _wrap);
  }

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
          if (i > 0) {
            status += "/";
            status_vwidth++;
          }
          auto h = hintFor(_completions[i].first);
          if (h.empty())
            h = _completions[i].first;
          status += h;
          status_vwidth += h.size();
        }
        status += "]\x1b[22m";
        status_vwidth++;
      } else {
        auto cnt = " (" + std::to_string(_completions.size()) + " matches)";
        status += "\x1b[2m" + hint + cnt + "\x1b[22m";
        status_vwidth += hint.size() + cnt.size();
      }
    }
  } else if (_prompt == Prompt::ConfirmSave || _prompt == Prompt::ConfirmKill ||
             _prompt == Prompt::ConfirmRevert) {
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
  _renderBuf += "\x1b[";
  _renderBuf += std::to_string(rows.value);
  _renderBuf += ";1H";
  _renderBuf += "\x1b[7m"; // reverse video
  _renderBuf += status;
  _renderBuf += "\x1b[0m"; // reset

  // Position the terminal cursor AFTER the status line, so it lands
  // in the buffer area rather than on the status bar.
  _renderBuf += "\x1b[";
  _renderBuf += std::to_string(cur_row.value + 1);
  _renderBuf += ";";
  _renderBuf += std::to_string(cur_col.value + 1);
  _renderBuf += "H";

  // Ensure the cursor is visible.
  _renderBuf += "\x1b[?25h";

  writeAll(STDOUT_FILENO, _renderBuf.data(), _renderBuf.size());
}

void Screen::loop() {
  char c;
  std::optional<char> pendingChar;
  while (true) {
    if (pendingChar) {
      c = *pendingChar;
      pendingChar.reset();
    } else {
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
      if (n != 1)
        break;

      // Also check after a successful read, in case the signal arrived
      // between loop iterations rather than during a blocked read().
      if (g_resized) {
        g_resized = 0;
        resize();
        draw();
      }
    }

    bool drew = false;
    if (_prompt == Prompt::ConfirmSave)
      drew = handleConfirmSave(c);
    else if (_prompt == Prompt::ConfirmKill)
      drew = handleConfirmKill(c);
    else if (_prompt == Prompt::ConfirmRevert)
      drew = handleConfirmRevert(c);
    else if (_prompt != Prompt::None)
      drew = handleTextPrompt(c);
    else
      drew = handleNormalInput(c, pendingChar);

    // Process pending exit: walk modified buffers or break.
    if (_exitPending && _prompt == Prompt::None) {
      if (tryExit())
        break;
    }

    if (!drew)
      draw();
  }
}

bool Screen::handleConfirmSave(char c) {
  if (c == 'y' || c == 'Y') {
    if (_buf) {
      auto res = _buf->save();
      if (res) {
        setStatus("Saved to " + _buf->name());
        if (_killPending && _bufMgr) {
          _killPending = false;
          auto name = _buf->name();
          _prompt = Prompt::None;
          _prompt_input.clear();
          if (_prevBuf == _buf)
            _prevBuf = nullptr;
          setBuffer(_bufMgr->kill(name));
          draw();
          return true;
        }
      } else {
        setStatus(res.error());
        _killPending = false;
      }
    }
    _prompt = Prompt::None;
    _prompt_input.clear();
  } else if (c == 'n' || c == 'N' || c == 0x1b) {
    _prompt = Prompt::None;
    _prompt_input.clear();
    _killPending = false;
    _exitPending = false;
  }
  // Ignore all other keys.
  return false;
}

bool Screen::handleConfirmKill(char c) {
  if (c == 'y' || c == 'Y') {
    if (_buf && _bufMgr) {
      if (_buf->hasPath()) {
        // Modified file-backed: save then kill.
        auto res = _buf->save();
        _prompt = Prompt::None;
        _prompt_input.clear();
        if (!res) {
          setStatus(res.error());
        } else {
          if (_prevBuf == _buf)
            _prevBuf = nullptr;
          setBuffer(_bufMgr->kill(_buf->name()));
        }
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
      if (_prevBuf == _buf)
        _prevBuf = nullptr;
      setBuffer(_bufMgr->kill(name));
    }
  } else if (c == 0x1b) {
    _prompt = Prompt::None;
    _prompt_input.clear();
    _exitPending = false;
  }
  return false;
}

bool Screen::handleConfirmRevert(char c) {
  if (c == 'y' || c == 'Y') {
    if (_buf) {
      auto res = _buf->revert();
      if (res)
        setStatus("Reverted from " + _buf->name());
      else
        setStatus(res.error());
    }
    _prompt = Prompt::None;
    _prompt_input.clear();
  } else if (c == 'n' || c == 'N' || c == 0x1b) {
    _prompt = Prompt::None;
    _prompt_input.clear();
  }
  return false;
}

bool Screen::handleTextPrompt(char c) {
  if (c == '\r' || c == '\n') {
    if (!_prompt_input.empty()) {
      if (_prompt == Prompt::SwitchBuffer && _bufMgr) {
        // Resolve target from completion or typed input.
        std::string resolved;
        if (_completionIdx >= 0 && _completionIdx < static_cast<int>(_completions.size()))
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
        } else if (_prompt_input[0] == '*') {
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
            return true;
          }
          setBuffer(_bufMgr->open(_prompt_input));
        }
      } else if (_prompt == Prompt::SaveAs && _buf && _bufMgr) {
        auto targetExists = std::filesystem::exists(_prompt_input);
        _bufMgr->rename(_buf->name(), _prompt_input);
        if (targetExists) {
          _prompt = Prompt::ConfirmSave;
          _prompt_input = "Overwrite " + _buf->name() + "? y/n: ";
          draw();
          return true;
        }
        auto res = _buf->save();
        if (res) {
          setStatus("Saved to " + _buf->name());
          if (_killPending) {
            _killPending = false;
            auto name = _buf->name();
            _prompt = Prompt::None;
            _prompt_input.clear();
            _completions.clear();
            _completionIdx = -1;
            if (_prevBuf == _buf)
              _prevBuf = nullptr;
            setBuffer(_bufMgr->kill(name));
            draw();
            return true;
          }
        } else {
          setStatus(res.error());
          _killPending = false;
        }
      } else if (_prompt == Prompt::GoToLine && _buf) {
        std::size_t line = 0;
        if (_prompt_input == "last") {
          line = std::numeric_limits<std::size_t>::max();
        } else {
          for (char ch : _prompt_input) {
            if (ch < '0' || ch > '9') {
              line = 0;
              break;
            }
            auto digit = static_cast<std::size_t>(ch - '0');
            if (line > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
              line = std::numeric_limits<std::size_t>::max();
              break;
            }
            line = line * 10 + digit;
          }
        }
        if (line > 0)
          _buf->gotoLine(line, rowsForBuffer());
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
      if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
          read(STDIN_FILENO, &seq[1], 1) == 1) {
        if (seq[1] == 'C' && _prompt == Prompt::SwitchBuffer && !_completions.empty()) {
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
  } else if (c == '\t' && _prompt == Prompt::SwitchBuffer && !_completions.empty()) {
    if (_completionIdx < 0)
      _completionIdx = 0;
    else
      _completionIdx = (_completionIdx + 1) % static_cast<int>(_completions.size());
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
    if (u >= 0x80)
      readUtf8Tail(u, _prompt_input);
    if (_prompt == Prompt::SwitchBuffer)
      updateCompletions();
  }
  return false;
}

bool Screen::handleEscapeInput(std::optional<char>& pendingChar) {
  auto& [rows, cols] = _size;
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 50) > 0) {
    // Escape sequence: read the next bytes to identify the key.
    char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
      if (read(STDIN_FILENO, &seq[1], 1) == 1) {
        switch (seq[1]) {
        case 'A':
          if (_buf) {
            if (_wrap)
              _buf->moveUp(cols);
            else
              _buf->movePhysicalUp();
            if (_buf->point() < _buf->viewFrom())
              _buf->scrollUp(_wrap);
          }
          break;
        case 'B':
          if (_buf) {
            auto prev = _buf->point();
            if (_wrap)
              _buf->moveDown(cols);
            else
              _buf->movePhysicalDown();
            if (_buf->point() == prev)
              _buf->scrollViewNearEnd(rowsForBuffer());
            else
              _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
          }
          break;
        case 'C':
          if (_buf)
            _buf->moveRight();
          break;
        case 'D':
          if (_buf)
            _buf->moveLeft();
          break;
        case 'H':
          if (_buf)
            _buf->moveHome();
          break;
        case 'F':
          if (_buf)
            _buf->moveEnd();
          break;
        case '1': {
          // Modified key: ESC[1;modX (e.g. Ctrl+Arrow = ESC[1;5X)
          char semi, mod, dir;
          if (read(STDIN_FILENO, &semi, 1) == 1 && semi == ';' &&
              read(STDIN_FILENO, &mod, 1) == 1 && read(STDIN_FILENO, &dir, 1) == 1) {
            if (mod == '5' && _buf) {
              switch (dir) {
              case 'C':
                _buf->moveWordRight();
                _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
                break;
              case 'D':
                _buf->moveWordLeft();
                while (_buf->point() < _buf->viewFrom())
                  _buf->scrollUp(_wrap);
                break;
              case 'A':
                if (_wrap && _buf->currentLineWraps(cols))
                  _buf->movePhysicalUp();
                else
                  _buf->moveParagraphUp();
                while (_buf->point() < _buf->viewFrom())
                  _buf->scrollUp(_wrap);
                break;
              case 'B':
                if (_wrap && _buf->currentLineWraps(cols))
                  _buf->movePhysicalDown();
                else
                  _buf->moveParagraphDown();
                _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
                break;
              }
            }
          }
          break;
        }
        case '3': {
          char next;
          if (read(STDIN_FILENO, &next, 1) == 1) {
            if (next == '~') {
              // Plain Delete: ESC[3~
              if (_buf && _buf->mark()) {
                _buf->deleteRegion(_buf->mark().value(), _buf->point());
                _pasteState.reset();
              } else if (_buf) {
                _buf->deleteAtPoint();
                _pasteState.reset();
              }
            } else if (next == ';') {
              // Modified Delete: ESC[3;mod~
              char mod, tilde;
              if (read(STDIN_FILENO, &mod, 1) == 1 && read(STDIN_FILENO, &tilde, 1) == 1 &&
                  tilde == '~') {
                if (mod == '5' && _buf)
                  _buf->deleteChunkAtPoint();
              }
            }
          }
          break;
        }
        case '5': {
          char tilde;
          if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~' && _buf)
            _buf->pageUp(rowsForBuffer(), cols, _wrap);
          break;
        }
        case '6': {
          char tilde;
          if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~' && _buf)
            _buf->pageDown(rowsForBuffer(), cols, _wrap);
          break;
        }
        default: {
          // Drain remaining bytes of unrecognised escape sequence.
          char discard;
          struct pollfd dp = {STDIN_FILENO, POLLIN, 0};
          while (poll(&dp, 1, 0) > 0)
            if (read(STDIN_FILENO, &discard, 1) != 1)
              break;
          break;
        }
        }
      }
    } else if (!handleEscChord(seq[0]) && seq[0] != '[') {
      // Not a CSI sequence or known chord — insert the byte verbatim
      // (e.g. Alt+key sends ESC followed by the key).
      if (_buf && static_cast<unsigned char>(seq[0]) >= 0x20)
        insertCharWithUtf8(*_buf, seq[0]);
    }
  } else {
    // Bare Esc — enter prefix mode: wait for the next key.
    setStatus("Esc-...");
    draw();

    char next = 0;
    bool gotNext = false;
    for (;;) {
      ssize_t nn = read(STDIN_FILENO, &next, 1);
      if (nn == 1) {
        gotNext = true;
        break;
      }
      if (nn == -1 && errno == EINTR) {
        if (g_resized) {
          g_resized = 0;
          resize();
        }
        // Redraw to keep "Esc-..." visible after resize.
        setStatus("Esc-...");
        draw();
        continue;
      }
      break; // EOF or error
    }

    if (gotNext && !handleEscChord(next)) {
      if (next == 0x1b) {
        // Esc-Esc: dismiss readonly overlay if active.
        if (_buf && _buf->readonly() && _prevBuf) {
          setBuffer(*_prevBuf);
          _prevBuf = nullptr;
        }
      } else {
        setStatus(formatShortcut(shortcutFromEscChar(next)) + " not recognised.");
      }
    }
    // Clear the "Esc-..." prompt, but only if the chord handler didn't
    // set its own status message.
    if (_statusMsg == "Esc-...") {
      _statusMsg.clear();
      _statusExpiry.reset();
    }
  }
  return false;
}

bool Screen::handleNormalInput(char c, std::optional<char>& pendingChar) {
  auto& [rows, cols] = _size;
  if (c == '\r' || c == '\n') {
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
      _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
      _pasteState.reset();
    }
  } else if (c == 0x7f) {
    // Backspace: if mark active, delete region (no paste ring push).
    if (_buf && _buf->mark()) {
      _buf->deleteRegion(_buf->mark().value(), _buf->point());
      _pasteState.reset();
    } else if (_buf) {
      _buf->deleteBeforePoint();
      _pasteState.reset();
    }
  } else if (c == 0x1b) {
    return handleEscapeInput(pendingChar);
  } else if (static_cast<unsigned char>(c) < 0x20) {
    auto s = shortcutFromCtrlChar(c);
    if (auto action = handleShortcut(s))
      dispatchAction(*action);
    else
      setStatus(formatShortcut(s) + " not recognised.");
  } else {
    if (_buf && _buf->readonly() && c == 'q' && _prevBuf) {
      setBuffer(*_prevBuf);
      _prevBuf = nullptr;
    } else if (_buf) {
      insertCharWithUtf8(*_buf, c);
      _pasteState.reset();
    }
  }
  return false;
}

RowIdx Screen::rowsForBuffer() const {
  auto& [rows, cols] = _size;
  return RowIdx{rows.value > 1 ? rows.value - 1 : 1};
}

void Screen::resize() {
  struct winsize ws = {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
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

bool Screen::handleEscChord(char ch) {
  auto s = shortcutFromEscChar(ch);
  if (auto action = handleShortcut(s)) {
    dispatchAction(*action);
    return true;
  }
  return false;
}

bool Screen::tryExit() {
  if (!_bufMgr)
    return true;

  // Dismiss any readonly overlay first.
  if (_buf && _buf->readonly() && _prevBuf) {
    setBuffer(*_prevBuf);
    _prevBuf = nullptr;
  }

  // Find the next modified, non-readonly buffer.
  for (auto& name : _bufMgr->bufferNames()) {
    Buffer* buf = _bufMgr->find(name);
    if (buf && buf->modified() && !buf->readonly()) {
      setBuffer(*buf);
      _prompt = Prompt::ConfirmKill;
      if (!buf->hasPath())
        _prompt_input = "Exiting. Save temporary buffer? y/n/Esc: ";
      else
        _prompt_input = "Exiting. Save " + _bufMgr->displayName(buf->name()) + "? y/n/Esc: ";
      return false;
    }
  }
  return true; // no modified buffers remain
}

void Screen::dispatchAction(Action action) {
  auto& [rows, cols] = _size;
  switch (action) {
  case Action::Help:
    if (_bufMgr) {
      if (_buf && _buf->readonly() && _prevBuf) {
        setBuffer(*_prevBuf);
        _prevBuf = nullptr;
      } else {
        _prevBuf = _buf;
        setBuffer(_bufMgr->helpBuffer());
      }
    }
    break;
  case Action::Exit:
    _exitPending = true;
    break;
  case Action::GoToLine:
    _prompt = Prompt::GoToLine;
    _prompt_input.clear();
    break;
  case Action::SwitchBuffer:
    _prevBuf = nullptr;
    _prompt = Prompt::SwitchBuffer;
    _prompt_input.clear();
    updateCompletions();
    break;
  case Action::KillBuffer:
    if (_buf && _bufMgr && !_buf->readonly()) {
      if (!_buf->modified()) {
        auto name = _buf->name();
        if (_prevBuf == _buf)
          _prevBuf = nullptr;
        setBuffer(_bufMgr->kill(name));
      } else if (!_buf->hasPath()) {
        _prompt = Prompt::ConfirmKill;
        _prompt_input = "Killing temporary buffer. Save to file first? y/n: ";
      } else {
        _prompt = Prompt::ConfirmKill;
        _prompt_input = "Killing modified buffer. Save to file first? y/n: ";
      }
    }
    break;
  case Action::Save:
    if (_buf && !_buf->readonly()) {
      if (_buf->hasPath()) {
        if (!std::filesystem::exists(_buf->name())) {
          _prompt = Prompt::ConfirmSave;
          _prompt_input = "File " + _buf->name() + " doesn't exist. Really save there? y/n: ";
        } else {
          auto res = _buf->save();
          if (res)
            setStatus("Saved to " + _buf->name());
          else
            setStatus(res.error());
        }
      } else {
        _prompt = Prompt::SaveAs;
        _prompt_input.clear();
      }
    }
    break;
  case Action::ToggleMark:
    if (_buf) {
      _buf->toggleMark();
      setStatus(_buf->mark() ? "Mark set" : "Mark cleared");
      _pasteState.reset();
    }
    break;
  case Action::Cut:
    if (_buf && _buf->mark()) {
      auto text = _buf->extractRegion(_buf->mark().value(), _buf->point());
      _pasteRing.push(std::move(text));
      _buf->deleteRegion(_buf->mark().value(), _buf->point());
      setStatus("Region cut");
      _pasteState.reset();
    } else {
      setStatus("No mark set");
    }
    break;
  case Action::Copy:
    if (_buf && _buf->mark()) {
      auto text = _buf->extractRegion(_buf->mark().value(), _buf->point());
      _pasteRing.push(std::move(text));
      _buf->clearMark();
      setStatus("Region copied");
      _pasteState.reset();
    } else {
      setStatus("No mark set");
    }
    break;
  case Action::Paste:
    if (_buf && !_buf->readonly() && !_pasteRing.empty()) {
      auto start = _buf->point();
      auto end = _buf->insertText(_pasteRing.at(0));
      _pasteState = PasteState{start, end, 0};
      _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
    }
    break;
  case Action::PasteCycle:
    if (_buf && !_buf->readonly() && _pasteState && !_pasteRing.empty()) {
      auto nextIdx = (_pasteState->rbIdx + 1) % _pasteRing.size();
      _buf->deleteRegion(_pasteState->start, _pasteState->end);
      auto start = _buf->point();
      auto end = _buf->insertText(_pasteRing.at(nextIdx));
      _pasteState = PasteState{start, end, nextIdx};
      setStatus("Cycling paste " + std::to_string(nextIdx + 1) + "/" +
                std::to_string(_pasteRing.size()));
      _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
    } else {
      setStatus("Can't cycle paste if last command was not a paste");
    }
    break;
  case Action::DeleteChunkBefore:
    if (_buf)
      _buf->deleteChunkBeforePoint();
    break;
  case Action::Revert:
    if (_buf && !_buf->readonly()) {
      if (!_buf->hasPath()) {
        setStatus("Can't revert from file in temporary buffers.");
      } else {
        _prompt = Prompt::ConfirmRevert;
        _prompt_input = "Really revert buffer from file " + _buf->name() + "? y/n: ";
      }
    }
    break;
  case Action::BufferList:
    if (_bufMgr) {
      if (_buf && _buf->readonly() && _prevBuf) {
        setBuffer(*_prevBuf);
        _prevBuf = nullptr;
      } else {
        _prevBuf = _buf;
        setBuffer(_bufMgr->bufferListBuffer());
      }
    }
    break;
  case Action::PasteRing:
    if (_bufMgr) {
      if (_buf && _buf->readonly() && _prevBuf) {
        setBuffer(*_prevBuf);
        _prevBuf = nullptr;
      } else {
        _prevBuf = _buf;
        setBuffer(_bufMgr->pasteRingBuffer(_pasteRing));
      }
    }
    break;
  case Action::ToggleWhitespace:
    _showWhitespace = !_showWhitespace;
    setStatus(_showWhitespace ? "Whitespace on" : "Whitespace off");
    break;
  case Action::ToggleWrap:
    _wrap = !_wrap;
    setStatus(_wrap ? "Wrap on" : "Wrap off");
    break;
  }
}
