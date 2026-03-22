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
  // Rough estimate: ~2 bytes per cell for content + ANSI escapes.
  std::size_t estimate = rows.value * cols.value * 2;
  if (_renderBuf.capacity() < estimate)
    _renderBuf.reserve(estimate);

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

  // Expire find highlight.
  if (_highlightExpiry && std::chrono::steady_clock::now() >= *_highlightExpiry) {
    if (_buf)
      _buf->clearHighlight();
    _highlightExpiry.reset();
  }

  // Status line on the last row, with inverted colours.
  // Always show line:col when a buffer is available.
  std::string status;
  std::size_t status_vwidth = 0; // visual width (excludes ANSI escapes)
  if (_buf) {
    auto [line, col] = _buf->lineCol();
    status = std::to_string(line) + ":" + std::to_string(col);
    status_vwidth = status.size();
  }
  if (_prompt == Prompt::GoToLine) {
    status += " | Go to line: " + _prompt_input;
    status_vwidth = status.size();
  } else if (_prompt == Prompt::SaveAs) {
    status += " | Save as: " + _prompt_input;
    status_vwidth = status.size();
  } else if (_prompt == Prompt::SwitchBuffer) {
    status += " | Buffer: " + _prompt_input;
    status_vwidth = status.size();
    // Append completion hints (dim text). A completion's match
    // string may or may not share the typed prefix (e.g. filename
    // matches against a full-path match string).
    auto hintFor = [&](std::string const& match) -> std::string {
      if (match.starts_with(_prompt_input))
        return match.substr(_prompt_input.size());
      return match;
    };
    if (!_completionIdx && !_completions.empty()) {
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
  } else if (_prompt == Prompt::Find) {
    status += " | Find: " + _prompt_input;
    status_vwidth = status.size();
    if (_prompt_input.empty() && !_lastSearch.empty()) {
      status += "\x1b[2m" + _lastSearch + "\x1b[22m";
      status_vwidth += _lastSearch.size();
    }
  } else if (_prompt == Prompt::ConfirmSave || _prompt == Prompt::ConfirmKill ||
             _prompt == Prompt::ConfirmRevert) {
    status += " | " + _prompt_input;
    status_vwidth = status.size();
  } else {
    if (!_statusMsg.empty()) {
      status += " | " + _statusMsg;
      status_vwidth = status.size();
    }
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

void Screen::submitSwitchBuffer() {
  if (!_bufMgr)
    return;
  // Resolve target from completion or typed input.
  std::string resolved;
  if (_completionIdx && *_completionIdx < _completions.size())
    resolved = _completions[*_completionIdx].second;
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
      return; // stay in prompt
    }
    setBuffer(_bufMgr->open(_prompt_input));
  }
}

void Screen::submitSaveAs() {
  if (!_buf || !_bufMgr)
    return;
  auto targetExists = std::filesystem::exists(_prompt_input);
  _bufMgr->rename(_buf->name(), _prompt_input);
  if (targetExists) {
    _prompt = Prompt::ConfirmSave;
    _prompt_input = "Overwrite " + _buf->name() + "? y/n: ";
    return; // transition to confirm prompt
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
      _completionIdx.reset();
      if (_prevBuf == _buf)
        _prevBuf = nullptr;
      setBuffer(_bufMgr->kill(name));
      return;
    }
  } else {
    setStatus(res.error());
    _killPending = false;
  }
}

void Screen::submitFind() {
  if (!_buf)
    return;
  _lastSearch = _prompt_input;
  findNext();
}

void Screen::submitGoToLine() {
  if (!_buf)
    return;
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

bool Screen::handleTextPrompt(char c) {
  if (c == '\r' || c == '\n') {
    if (!_prompt_input.empty()) {
      if (_prompt == Prompt::SwitchBuffer) {
        submitSwitchBuffer();
        // submitSwitchBuffer may stay in prompt (directory navigation).
        if (_prompt != Prompt::None) {
          draw();
          return true;
        }
      } else if (_prompt == Prompt::SaveAs) {
        submitSaveAs();
        // submitSaveAs may transition to ConfirmSave or kill-after-save.
        if (_prompt != Prompt::None) {
          draw();
          return true;
        }
      } else if (_prompt == Prompt::Find) {
        submitFind();
      } else if (_prompt == Prompt::GoToLine) {
        submitGoToLine();
      }
    }
    _prompt = Prompt::None;
    _prompt_input.clear();
    _completions.clear();
    _completionIdx.reset();
  } else if (c == 0x1b) {
    // Distinguish bare Esc (cancel) from escape sequences.
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 50) > 0) {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
          read(STDIN_FILENO, &seq[1], 1) == 1) {
        if (seq[1] == 'C' && _prompt == Prompt::SwitchBuffer && !_completions.empty()) {
          // Right arrow: accept the current completion.
          if (_completionIdx)
            _prompt_input = _completions[*_completionIdx].first;
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
      _completionIdx.reset();
      _killPending = false;
    }
  } else if (c == '\t' && _prompt == Prompt::SwitchBuffer && !_completions.empty()) {
    if (!_completionIdx)
      _completionIdx = 0;
    else
      _completionIdx = (*_completionIdx + 1) % _completions.size();
    _prompt_input = _completions[*_completionIdx].first;
    // Directory: navigate into it only when unambiguous.
    if (_prompt_input.ends_with('/') && _completions.size() == 1)
      updateCompletions();
  } else if (c == 0x7f && !_prompt_input.empty()) {
    // Remove one full UTF-8 code point (walk back over continuation bytes).
    _prompt_input.pop_back();
    while (!_prompt_input.empty() &&
           isUtf8Continuation(static_cast<unsigned char>(_prompt_input.back())))
      _prompt_input.pop_back();
    if (_prompt == Prompt::SwitchBuffer)
      updateCompletions();
  } else if (_prompt == Prompt::Find && c == 0x06) {
    // Ctrl-F while in Find prompt: advance to next match forward.
    if (!_prompt_input.empty()) {
      _lastSearch = _prompt_input;
      _prompt_input.clear();
    }
    if (!_lastSearch.empty())
      findNext();
  } else if (_prompt == Prompt::Find && c == 0x12) {
    // Ctrl-R while in Find prompt: advance to next match backward.
    if (!_prompt_input.empty()) {
      _lastSearch = _prompt_input;
      _prompt_input.clear();
    }
    if (!_lastSearch.empty())
      findPrev();
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
              case 'H':
                _buf->setPoint(0, 0);
                _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
                break;
              case 'F':
                _buf->setPoint(_buf->lineCount() - 1,
                               _buf->lineContent(_buf->lineCount() - 1).size());
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
  if (_buf && _highlightExpiry) {
    _buf->clearHighlight();
    _highlightExpiry.reset();
  }
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
    _completionIdx.reset();
  } else {
    _completions.clear();
    _completionIdx.reset();
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

// Check if `pattern` matches `line` starting at byte offset `off`.
// Ignored characters in the buffer line are skipped between matched chars,
// but the first pattern character must match directly at `off`.
// Returns the byte offset past the last matched character, or nullopt on no match.
//
// Case folding is ASCII-only: non-ASCII uppercase letters (e.g. Ü) will not
// match their lowercase counterparts. Multi-byte UTF-8 patterns match by raw
// byte comparison, which works for exact matches but not case-insensitive ones.
static std::optional<std::size_t> matchAt(std::vector<char> const& line, std::size_t off,
                                          std::string const& pattern, bool caseSensitive,
                                          bool matchDigits, bool matchSpecial) {
  if (pattern.empty())
    return off;
  auto lower = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  };

  std::size_t pi = 0, li = off;

  // First character must match directly (no skipping).
  if (li >= line.size())
    return std::nullopt;
  if (caseSensitive ? (pattern[pi] != line[li]) : (lower(pattern[pi]) != lower(line[li])))
    return std::nullopt;
  ++pi;
  ++li;

  // Remaining characters: skip ignored chars between matches.
  while (pi < pattern.size()) {
    while (li < line.size()) {
      auto u = static_cast<unsigned char>(line[li]);
      if (u >= 0x80) {
        if (!matchSpecial) {
          li += utf8SeqLen(u);
          continue;
        }
        break;
      }
      bool isLetter = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
      bool isDigit = (u >= '0' && u <= '9');
      if (!matchDigits && isDigit) {
        ++li;
        continue;
      }
      if (!matchSpecial && !isLetter && !isDigit) {
        ++li;
        continue;
      }
      break;
    }
    if (li >= line.size())
      return std::nullopt;
    if (caseSensitive ? (pattern[pi] != line[li]) : (lower(pattern[pi]) != lower(line[li])))
      return std::nullopt;
    ++pi;
    ++li;
  }
  return li;
}

// Determine matching flags from the search pattern. Case-insensitive
// unless the pattern contains uppercase; digits and special chars in the
// buffer are skipped unless they appear in the pattern (ASCII-only).
struct FindFlags {
  bool caseSensitive = false;
  bool matchDigits = false;
  bool matchSpecial = false;
  char firstChar = 0;  // exact first char of pattern
  char firstLower = 0; // lowercased
  char firstUpper = 0; // uppercased

  explicit FindFlags(std::string_view pattern) {
    for (auto c : pattern) {
      auto u = static_cast<unsigned char>(c);
      if (u >= 'A' && u <= 'Z')
        caseSensitive = true;
      if (u >= '0' && u <= '9')
        matchDigits = true;
      bool isLetter = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
      bool isDigit = (u >= '0' && u <= '9');
      if (!isLetter && !isDigit)
        matchSpecial = true;
    }
    firstChar = pattern[0];
    auto lower = [](char c) -> char {
      return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    firstLower = lower(firstChar);
    firstUpper =
        (firstLower >= 'a' && firstLower <= 'z') ? static_cast<char>(firstLower - 32) : firstLower;
  }

  bool firstCharCanMatch(char c) const {
    if (caseSensitive)
      return c == firstChar;
    return c == firstLower || c == firstUpper;
  }
};

// Scan a line for the first match starting at or after `from`.
static std::optional<std::pair<std::size_t, std::size_t>>
findFirstMatch(std::vector<char> const& line, std::size_t from, std::string const& pattern,
               FindFlags const& f) {
  for (std::size_t off = from; off < line.size(); ++off) {
    if (!f.firstCharCanMatch(line[off]))
      continue;
    if (auto end = matchAt(line, off, pattern, f.caseSensitive, f.matchDigits, f.matchSpecial))
      return std::pair{off, *end};
  }
  return std::nullopt;
}

// Scan a line for the last match within [from, limit).
static std::optional<std::pair<std::size_t, std::size_t>>
findLastMatch(std::vector<char> const& line, std::size_t from, std::size_t limit,
              std::string const& pattern, FindFlags const& f) {
  std::optional<std::pair<std::size_t, std::size_t>> last;
  for (std::size_t off = from; off < limit && off < line.size(); ++off) {
    if (!f.firstCharCanMatch(line[off]))
      continue;
    if (auto end = matchAt(line, off, pattern, f.caseSensitive, f.matchDigits, f.matchSpecial))
      last = {off, *end};
  }
  return last;
}

void Screen::onFindMatch(std::size_t line, std::size_t matchStart, std::size_t matchEnd,
                         bool wrapped) {
  auto& [rows, cols] = _size;
  _buf->setPoint(line, matchEnd);
  _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
  _buf->setHighlight({line, matchStart}, {line, matchEnd});
  _highlightExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  _pasteState.reset();
  if (wrapped)
    setStatus("Found (wrapped)");
  else {
    _statusMsg.clear();
    _statusExpiry.reset();
  }
}

void Screen::findNext() {
  if (!_buf || _lastSearch.empty())
    return;
  FindFlags f(_lastSearch);

  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _buf->point().line;
  std::size_t startOff = _buf->point().offset;
  bool wrapped = false;

  // Search forward from current position, wrapping around.
  for (std::size_t count = 0; count < lineCount; ++count) {
    std::size_t li = (startLine + count) % lineCount;
    if (count > 0 && li <= startLine)
      wrapped = true;
    std::size_t from = (count == 0) ? startOff : 0;
    if (auto m = findFirstMatch(_buf->lineContent(li), from, _lastSearch, f)) {
      onFindMatch(li, m->first, m->second, wrapped);
      return;
    }
  }

  // Check the start of the starting line (before original cursor).
  if (auto m = findFirstMatch(_buf->lineContent(startLine), 0, _lastSearch, f)) {
    if (m->first < startOff) {
      onFindMatch(startLine, m->first, m->second, true);
      return;
    }
  }

  _buf->clearHighlight();
  _highlightExpiry.reset();
  setStatus("Not found");
}

void Screen::findPrev() {
  if (!_buf || _lastSearch.empty())
    return;
  FindFlags f(_lastSearch);

  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _buf->point().line;
  std::size_t startOff = _buf->point().offset;
  bool wrapped = false;

  // Search backward on the current line before the cursor.
  // Strict < on end so we skip the current match (whose end == startOff).
  if (auto m = findLastMatch(_buf->lineContent(startLine), 0, startOff, _lastSearch, f)) {
    if (m->second < startOff) {
      onFindMatch(startLine, m->first, m->second, false);
      return;
    }
  }

  // Search backward through preceding lines, wrapping around.
  for (std::size_t count = 1; count < lineCount; ++count) {
    std::size_t li = (startLine + lineCount - count) % lineCount;
    if (count > startLine)
      wrapped = true;
    auto const& line = _buf->lineContent(li);
    if (auto m = findLastMatch(line, 0, line.size(), _lastSearch, f)) {
      onFindMatch(li, m->first, m->second, wrapped);
      return;
    }
  }

  // Check the current line after the cursor (wrapped).
  {
    auto const& line = _buf->lineContent(startLine);
    if (auto m = findLastMatch(line, startOff, line.size(), _lastSearch, f)) {
      onFindMatch(startLine, m->first, m->second, true);
      return;
    }
  }

  _buf->clearHighlight();
  _highlightExpiry.reset();
  setStatus("Not found");
}

void Screen::dispatchAction(Action action) {
  if (action != Action::Find && action != Action::FindBackward && _buf) {
    _buf->clearHighlight();
    _highlightExpiry.reset();
  }
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
  case Action::Find:
    if (!_lastSearch.empty() && _buf)
      findNext();
    _prompt = Prompt::Find;
    _prompt_input.clear();
    break;
  case Action::FindBackward:
    if (!_lastSearch.empty() && _buf)
      findPrev();
    _prompt = Prompt::Find;
    _prompt_input.clear();
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
