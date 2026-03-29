#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_manager.hpp"
#include "byte_constants.hpp"
#include "utf8.hpp"
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>

// Read a full UTF-8 character from stdin (given the lead byte) and insert
// it into the buffer as a single codepoint.
static void insertCharWithUtf8(Buffer& buf, char c) {
  buf.insert(readUtf8Codepoint(static_cast<unsigned char>(c)));
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
  if (_bufMgr && (!buf.readonly() || dynamic_cast<DirBuffer*>(&buf)))
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
    status_vwidth = visualWidth(status);
  }
  if (_prompt) {
    status += " | " + _prompt->label;
    if (_prompt->showInput)
      status += _prompt->input;
    status_vwidth = visualWidth(status);
    status += _prompt->display.rendered;
    status_vwidth += _prompt->display.visualWidth;
  } else if (!_statusMsg.empty()) {
    status += " | " + _statusMsg;
    status_vwidth = visualWidth(status);
  }

  // Right-align the buffer name + mode + modification indicator on the status line.
  std::string buf_name;
  if (_buf) {
    buf_name = _bufMgr ? _bufMgr->displayName(_buf->name()) : _buf->name();
    if (!_buf->readonly())
      buf_name += _buf->mode() == BufferMode::Cpp  ? " [c]"
                  : _buf->mode() == BufferMode::Md ? " [md]"
                                                   : " [txt]";
    if (_buf->modified())
      buf_name += " +";
    else
      buf_name += "  ";
  }
  auto buf_name_vwidth = visualWidth(buf_name);
  if (status_vwidth + buf_name_vwidth + 2 <= cols.value) {
    status.append(cols.value - buf_name_vwidth - status_vwidth, ' ');
    status += buf_name;
  } else if (status_vwidth < cols.value) {
    status.append(cols.value - status_vwidth, ' ');
  }
  _renderBuf += ansi::cursorTo(rows.value - 1, 0);
  _renderBuf += ansi::ReverseVideo;
  _renderBuf += status;
  _renderBuf += ansi::Reset;

  // Position the terminal cursor AFTER the status line, so it lands
  // in the buffer area rather than on the status bar.
  _renderBuf += ansi::cursorTo(cur_row.value, cur_col.value);

  // Ensure the cursor is visible.
  _renderBuf += ansi::ShowCursor;

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
    if (_prompt)
      drew = handlePromptInput(c);
    else
      drew = handleNormalInput(c, pendingChar);

    // Process pending exit: walk modified buffers or break.
    if (_exitPending && !_prompt) {
      if (tryExit())
        break;
    }

    if (!drew)
      draw();
  }
}

bool Screen::handlePromptInput(char c) {
  if (c == key::Enter || c == key::Newline) {
    if (!_prompt->showInput)
      return false; // Enter is ignored on confirm prompts
    if (!_prompt->input.empty()) {
      _promptHistory.push(_prompt->label, _prompt->input);
      auto resultFn = std::move(_prompt->onResult);
      auto value = std::move(_prompt->input);
      _prompt.reset();
      resultFn(value);
    } else {
      _prompt.reset();
    }
  } else if (c == key::Escape) {
    // Distinguish bare Esc (cancel) from escape sequences.
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 50) > 0) {
      char seq[2];
      if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == ansi::CsiIntro &&
          read(STDIN_FILENO, &seq[1], 1) == 1) {
        if (seq[1] == ansi::UpArrow) {
          // History: browse backward.
          auto* hist = _promptHistory.get(_prompt->label);
          if (hist) {
            if (!_prompt->historyIdx) {
              _prompt->savedInput = _prompt->input;
              _prompt->historyIdx = 0;
              _prompt->input = (*hist)[0];
            } else if (*_prompt->historyIdx + 1 < hist->size()) {
              ++*_prompt->historyIdx;
              _prompt->input = (*hist)[*_prompt->historyIdx];
            }
            runAutocomplete();
          }
        } else if (seq[1] == ansi::DownArrow) {
          // History: browse forward.
          if (_prompt->historyIdx) {
            if (*_prompt->historyIdx == 0) {
              _prompt->input = _prompt->savedInput;
              _prompt->historyIdx.reset();
            } else {
              --*_prompt->historyIdx;
              auto* hist = _promptHistory.get(_prompt->label);
              if (hist)
                _prompt->input = (*hist)[*_prompt->historyIdx];
            }
            runAutocomplete();
          }
        } else if (seq[1] == ansi::RightArrow && _prompt->onRightArrow) {
          _prompt->display = _prompt->onRightArrow(_prompt->input);
        }
        // Other escape sequences: ignored in prompt mode.
      }
    } else {
      // Bare Esc: cancel prompt.
      auto abortFn = std::move(_prompt->onAbort);
      _prompt.reset();
      if (abortFn)
        abortFn();
    }
  } else if (c == key::Tab && _prompt->onTab) {
    _prompt->display = _prompt->onTab(_prompt->input);
    _prompt->historyIdx.reset();
  } else if (c == key::Backspace && !_prompt->input.empty()) {
    // Remove one full UTF-8 code point (walk back over continuation bytes).
    _prompt->input.pop_back();
    while (!_prompt->input.empty() &&
           isUtf8Continuation(static_cast<unsigned char>(_prompt->input.back())))
      _prompt->input.pop_back();
    _prompt->historyIdx.reset();
    runAutocomplete();
  } else if (static_cast<unsigned char>(c) >= key::PrintableMin) {
    _prompt->input += c;
    auto u = static_cast<unsigned char>(c);
    if (u >= 0x80)
      readUtf8Tail(u, _prompt->input);
    _prompt->historyIdx.reset();
    if (runAutocomplete())
      return false; // Prompt was closed by early-exit result
  } else if (_prompt->onSpecialKey) {
    if (_prompt->onSpecialKey(c, _prompt->input))
      runAutocomplete();
  }
  return false;
}

bool Screen::runAutocomplete() {
  if (!_prompt || !_prompt->autocomplete)
    return false;
  auto result = _prompt->autocomplete(_prompt->input);
  if (auto* d = std::get_if<PromptDisplay>(&result)) {
    _prompt->display = *d;
    return false;
  }
  // Early exit: autocomplete returned a result value.
  auto& value = std::get<std::string>(result);
  if (_prompt->showInput)
    _promptHistory.push(_prompt->label, _prompt->input);
  auto resultFn = std::move(_prompt->onResult);
  _prompt.reset();
  resultFn(value);
  return true;
}

// --- Prompt construction helpers ---

void Screen::openGoToLinePrompt() {
  _prompt = Prompt{
      .label = "Go to line: ",
      .onResult =
          [this](std::string const& value) {
            if (!_buf)
              return;
            std::size_t line = 0;
            if (value == "last") {
              line = std::numeric_limits<std::size_t>::max();
            } else {
              for (char ch : value) {
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
          },
  };
}

void Screen::openSaveAsPrompt() {
  _prompt = Prompt{
      .label = "Save as: ",
      .onResult =
          [this](std::string const& value) {
            if (!_buf || !_bufMgr)
              return;
            if (std::filesystem::exists(value)) {
              // Defer rename until the user confirms the overwrite.
              auto savedName = _buf->name();
              openConfirmPrompt(
                  "Overwrite " + value + "? y/n: ",
                  [this, savedName, value](std::string const& answer) {
                    if (answer == "y") {
                      if (_buf && _bufMgr) {
                        _bufMgr->rename(savedName, value);
                        saveAndMaybeKill();
                      }
                    } else {
                      _killPending = false;
                      _exitPending = false;
                    }
                  },
                  [this]() {
                    _killPending = false;
                    _exitPending = false;
                  });
              return;
            }
            _bufMgr->rename(_buf->name(), value);
            saveAndMaybeKill();
          },
      .onAbort = [this]() { _killPending = false; },
  };
}

void Screen::openSwitchBufferPrompt() {
  struct CompletionState {
    std::vector<std::pair<std::string, std::string>> completions; // {display, bufName}
    std::optional<std::size_t> idx;
  };
  auto state = std::make_shared<CompletionState>();
  if (_bufMgr)
    state->completions = _bufMgr->completions("");

  // Build a PromptDisplay from the current completion state.
  auto buildDisplay = [state](std::string const& input) -> PromptDisplay {
    auto const& completions = state->completions;
    if (state->idx || completions.empty())
      return {"", 0};
    auto hintFor = [&](std::string const& match) -> std::string {
      if (match.starts_with(input))
        return match.substr(input.size());
      return match;
    };
    auto hint = hintFor(completions[0].first);
    if (completions.size() == 1) {
      if (hint.empty())
        return {"", 0};
      return {ansi::Dim + hint + ansi::DimOff, visualWidth(hint)};
    }
    if (completions.size() <= 3) {
      std::string rendered = std::string(ansi::Dim) + "[";
      std::size_t vw = 1;
      for (std::size_t i = 0; i < completions.size(); ++i) {
        if (i > 0) {
          rendered += "/";
          vw++;
        }
        auto h = hintFor(completions[i].first);
        if (h.empty())
          h = completions[i].first;
        rendered += h;
        vw += visualWidth(h);
      }
      rendered += std::string("]") + ansi::DimOff;
      vw++;
      return {std::move(rendered), vw};
    }
    auto cnt = " (" + std::to_string(completions.size()) + " matches)";
    return {ansi::Dim + hint + cnt + ansi::DimOff, visualWidth(hint) + cnt.size()};
  };

  _prompt = Prompt{
      .label = "Buffer: ",
      .autocomplete = [this, state,
                       buildDisplay](std::string const& input) -> Prompt::AutocompleteResult {
        if (_bufMgr)
          state->completions = _bufMgr->completions(input);
        state->idx.reset();
        return buildDisplay(input);
      },
      .onResult =
          [this, state](std::string const& value) {
            if (!_bufMgr)
              return;
            // Resolve target from completion or typed input.
            std::string resolved;
            if (state->idx && *state->idx < state->completions.size())
              resolved = state->completions[*state->idx].second;
            else if (state->completions.size() == 1)
              resolved = state->completions[0].second;
            Buffer* target = nullptr;
            if (!resolved.empty())
              target = _bufMgr->find(resolved);
            if (!target)
              target = _bufMgr->find(value);
            if (target) {
              setBuffer(*target);
            } else if (!value.empty() && value[0] == '*') {
              std::string name = value;
              if (name.back() != '*')
                name += '*';
              target = _bufMgr->find(name);
              if (target)
                setBuffer(*target);
              else
                setBuffer(_bufMgr->create(name));
            } else {
              std::error_code fec;
              if (std::filesystem::is_directory(value, fec))
                setBuffer(_bufMgr->openDir(value));
              else
                setBuffer(_bufMgr->open(value));
            }
          },
      .onTab = [this, state, buildDisplay](std::string& input) -> PromptDisplay {
        if (state->completions.empty())
          return PromptDisplay{"", 0};
        if (!state->idx)
          state->idx = 0;
        else
          state->idx = (*state->idx + 1) % state->completions.size();
        input = state->completions[*state->idx].first;
        // Directory: navigate into it only when unambiguous.
        if (input.ends_with('/') && state->completions.size() == 1) {
          if (_bufMgr)
            state->completions = _bufMgr->completions(input);
          state->idx.reset();
        }
        return buildDisplay(input);
      },
      .onRightArrow = [this, state, buildDisplay](std::string& input) -> PromptDisplay {
        if (state->completions.empty())
          return PromptDisplay{"", 0};
        if (state->idx)
          input = state->completions[*state->idx].first;
        else if (state->completions.size() == 1)
          input = state->completions[0].first;
        if (_bufMgr)
          state->completions = _bufMgr->completions(input);
        state->idx.reset();
        return buildDisplay(input);
      },
  };
  runAutocomplete();
}

void Screen::openFindPrompt(bool backward) {
  if (!_lastSearch.empty() && _buf) {
    if (backward)
      findPrev();
    else
      findNext();
  }
  _prompt = Prompt{
      .label = "Find: ",
      .autocomplete = [this](std::string const& input) -> Prompt::AutocompleteResult {
        if (input.empty() && !_lastSearch.empty())
          return PromptDisplay{ansi::Dim + _lastSearch + ansi::DimOff, visualWidth(_lastSearch)};
        return PromptDisplay{"", 0};
      },
      .onResult =
          [this](std::string const& value) {
            if (!_buf)
              return;
            _lastSearch = value;
            findNext();
          },
      .onSpecialKey = [this](char key, std::string& input) -> bool {
        if (key == key::CtrlF) {
          if (!input.empty()) {
            _lastSearch = input;
            input.clear();
          }
          if (!_lastSearch.empty())
            findNext();
          return true;
        }
        if (key == key::CtrlR) {
          if (!input.empty()) {
            _lastSearch = input;
            input.clear();
          }
          if (!_lastSearch.empty())
            findPrev();
          return true;
        }
        return false;
      },
  };
  runAutocomplete();
}

void Screen::openSetModePrompt() {
  _prompt = Prompt{
      .label = "Mode [txt/c/md]: ",
      .onResult =
          [this](std::string const& value) {
            if (value == "c" || value == "cpp" || value == "c++") {
              if (_buf)
                _buf->setMode(BufferMode::Cpp);
              setStatus("Mode: c");
            } else if (value == "md" || value == "markdown") {
              if (_buf)
                _buf->setMode(BufferMode::Md);
              setStatus("Mode: md");
            } else if (value == "txt" || value == "text") {
              if (_buf)
                _buf->setMode(BufferMode::Txt);
              setStatus("Mode: txt");
            } else {
              setStatus("Unknown mode: " + value + " (use 'txt', 'c', or 'md')");
            }
          },
  };
}

void Screen::openConfirmPrompt(std::string question,
                               std::function<void(std::string const&)> onResult,
                               std::function<void()> onAbort) {
  _prompt = Prompt{
      .label = std::move(question),
      .showInput = false,
      .autocomplete = [](std::string const& input) -> Prompt::AutocompleteResult {
        if (input.empty())
          return PromptDisplay{"", 0};
        char last = input.back();
        if (last == 'y' || last == 'Y')
          return std::string{"y"};
        if (last == 'n' || last == 'N')
          return std::string{"n"};
        return PromptDisplay{"", 0};
      },
      .onResult = std::move(onResult),
      .onAbort = std::move(onAbort),
  };
  runAutocomplete();
}

void Screen::saveAndMaybeKill() {
  if (!_buf)
    return;
  auto res = _buf->save();
  if (res) {
    setStatus("Saved to " + _buf->name());
    if (_killPending && _bufMgr) {
      _killPending = false;
      auto name = _buf->name();
      if (_prevBuf == _buf)
        _prevBuf = nullptr;
      setBuffer(_bufMgr->kill(name));
    }
  } else {
    setStatus(res.error());
    _killPending = false;
  }
}

void Screen::openConfirmKillPrompt(std::string question) {
  openConfirmPrompt(
      std::move(question),
      [this](std::string const& answer) {
        if (answer == "y") {
          // Save then kill.
          if (_buf && _bufMgr) {
            if (_buf->hasPath()) {
              auto res = _buf->save();
              if (!res) {
                setStatus(res.error());
              } else {
                if (_prevBuf == _buf)
                  _prevBuf = nullptr;
                setBuffer(_bufMgr->kill(_buf->name()));
              }
            } else {
              _killPending = true;
              openSaveAsPrompt();
            }
          }
        } else {
          // Kill without saving.
          if (_buf && _bufMgr) {
            auto name = _buf->name();
            if (_prevBuf == _buf)
              _prevBuf = nullptr;
            setBuffer(_bufMgr->kill(name));
          }
        }
      },
      [this]() { _exitPending = false; });
}

// --- Subprocess execution with kill-prompt support ---

static void drainFd(int fd, std::string& out) {
  char buf[4096];
  for (;;) {
    auto n = read(fd, buf, sizeof(buf));
    if (n > 0)
      out.append(buf, static_cast<std::size_t>(n));
    else if (n < 0 && errno == EINTR)
      continue;
    else
      break;
  }
}

std::string Screen::bufferWorkDir() const {
  if (_buf && _buf->hasPath())
    return std::filesystem::path(_buf->name()).parent_path().string();
  return std::filesystem::current_path().string();
}

std::optional<Screen::PipeResult> Screen::runPipeCommand(std::vector<std::string> const& args,
                                                         std::string_view input,
                                                         std::string const& workDir) {
  if (args.empty())
    return std::nullopt;

  int pipeIn[2], pipeOut[2], pipeErr[2];
  if (pipe(pipeIn) != 0) {
    setStatus(std::string("pipe: ") + std::strerror(errno));
    return std::nullopt;
  }
  if (pipe(pipeOut) != 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    setStatus(std::string("pipe: ") + std::strerror(errno));
    return std::nullopt;
  }
  if (pipe(pipeErr) != 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    setStatus(std::string("pipe: ") + std::strerror(errno));
    return std::nullopt;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    close(pipeErr[0]);
    close(pipeErr[1]);
    setStatus(std::string("fork: ") + std::strerror(errno));
    return std::nullopt;
  }

  if (pid == 0) {
    // Child: wire up pipes and exec.
    if (!workDir.empty())
      (void)chdir(workDir.c_str());
    dup2(pipeIn[0], STDIN_FILENO);
    dup2(pipeOut[1], STDOUT_FILENO);
    dup2(pipeErr[1], STDERR_FILENO);
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    close(pipeErr[0]);
    close(pipeErr[1]);

    std::vector<char*> argv;
    for (auto& a : args)
      argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    auto msg = std::string("exec: ") + std::strerror(errno) + "\n";
    (void)write(STDERR_FILENO, msg.data(), msg.size());
    _exit(127);
  }

  // Parent: close child-side pipe ends.
  close(pipeIn[0]);
  close(pipeOut[1]);
  close(pipeErr[1]);

  std::string stdoutData, stderrData;
  std::size_t written = 0;
  bool stdinOpen = true;
  bool killPromptShowing = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

  // Read one chunk from an output pipe into the target string; set done=true on EOF.
  auto readPipe = [](int fd, short revents, std::string& out, bool& done) {
    if (revents & (POLLIN | POLLHUP)) {
      char buf[4096];
      auto n = read(fd, buf, sizeof(buf));
      if (n > 0)
        out.append(buf, static_cast<std::size_t>(n));
      else if (n == 0)
        done = true;
    }
  };

  // Drain output pipes, close them, reap child, clear status, and return a killed result.
  auto killReturn = [&]() -> PipeResult {
    drainFd(pipeOut[0], stdoutData);
    drainFd(pipeErr[0], stderrData);
    close(pipeOut[0]);
    close(pipeErr[0]);
    _statusMsg.clear();
    _statusExpiry.reset();
    return {std::move(stdoutData), std::move(stderrData), -1, true};
  };

  // Poll set is rebuilt each iteration because stdinOpen and killPromptShowing can change.
  std::vector<pollfd> fds;
  fds.reserve(4);

  for (;;) {
    fds.clear();
    if (stdinOpen)
      fds.push_back({pipeIn[1], POLLOUT, 0});
    fds.push_back({pipeOut[0], POLLIN, 0});
    fds.push_back({pipeErr[0], POLLIN, 0});
    if (killPromptShowing)
      fds.push_back({STDIN_FILENO, POLLIN, 0});

    int timeoutMs = -1;
    if (!killPromptShowing) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      timeoutMs = std::max(0, static_cast<int>(remaining.count()));
    }

    int rc = poll(fds.data(), fds.size(), timeoutMs);
    if (rc < 0) {
      if (errno == EINTR) {
        if (g_resized) {
          g_resized = 0;
          resize();
          draw();
        }
        continue;
      }
      break;
    }

    if (rc == 0) {
      // Timeout: show kill prompt.
      setStatus("Command still running. Kill? y/n");
      draw();
      killPromptShowing = true;
      continue;
    }

    bool outDone = false, errDone = false;
    for (auto& pfd : fds) {
      if (pfd.fd == pipeIn[1] && (pfd.revents & (POLLOUT | POLLERR))) {
        if (written < input.size()) {
          auto n = write(pipeIn[1], input.data() + written, input.size() - written);
          if (n > 0)
            written += static_cast<std::size_t>(n);
          else if (n < 0 && errno != EINTR) {
            close(pipeIn[1]);
            stdinOpen = false;
          }
        } else {
          close(pipeIn[1]);
          stdinOpen = false;
        }
      }
      if (pfd.fd == pipeOut[0])
        readPipe(pipeOut[0], pfd.revents, stdoutData, outDone);
      if (pfd.fd == pipeErr[0])
        readPipe(pipeErr[0], pfd.revents, stderrData, errDone);
      if (pfd.fd == STDIN_FILENO && (pfd.revents & POLLIN)) {
        char ch;
        if (read(STDIN_FILENO, &ch, 1) == 1) {
          if (ch == 'y' || ch == 'Y') {
            // Give the process 1s to clean up after SIGTERM before forcing SIGKILL.
            kill(pid, SIGTERM);
            if (stdinOpen) {
              close(pipeIn[1]);
              stdinOpen = false;
            }
            for (int i = 0; i < 10; ++i) {
              int wstatus;
              if (waitpid(pid, &wstatus, WNOHANG) > 0)
                return killReturn();
              pollfd drain[] = {{pipeOut[0], POLLIN, 0}, {pipeErr[0], POLLIN, 0}};
              poll(drain, 2, 100);
              bool dummy = false;
              readPipe(pipeOut[0], drain[0].revents, stdoutData, dummy);
              readPipe(pipeErr[0], drain[1].revents, stderrData, dummy);
            }
            kill(pid, SIGKILL);
            {
              int wstatus;
              waitpid(pid, &wstatus, 0);
            }
            return killReturn();
          } else if (ch == 'n' || ch == 'N') {
            _statusMsg.clear();
            _statusExpiry.reset();
            draw();
            killPromptShowing = false;
            deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
          }
          // Ignore other characters.
        }
      }
    }

    // If child closed both output pipes, no point keeping stdin open.
    if (outDone && errDone && stdinOpen) {
      close(pipeIn[1]);
      stdinOpen = false;
    }

    if (outDone && errDone && !stdinOpen)
      break;
  }

  if (stdinOpen)
    close(pipeIn[1]);

  drainFd(pipeOut[0], stdoutData);
  drainFd(pipeErr[0], stderrData);
  close(pipeOut[0]);
  close(pipeErr[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  _statusMsg.clear();
  _statusExpiry.reset();
  return PipeResult{std::move(stdoutData), std::move(stderrData), exitCode, false};
}

void Screen::executeFormat(std::optional<std::pair<std::size_t, std::size_t>> lineRange) {
  if (!_buf)
    return;
  if (_buf->readonly())
    return;
  if (!_buf->hasPath()) {
    setStatus("Save file first to determine format");
    return;
  }
  if (_buf->mode() != BufferMode::Cpp) {
    setStatus("No formatter for this file type");
    return;
  }

  BufferPoint endPt{_buf->lineCount() - 1, _buf->lineContent(_buf->lineCount() - 1).size()};
  auto allText = _buf->extractRegion({0, 0}, endPt);

  std::vector<std::string> args = {"clang-format", "--assume-filename=" + _buf->name()};
  if (lineRange) {
    auto [startLine, endLine] = *lineRange;
    args.push_back("--lines=" + std::to_string(startLine) + ":" + std::to_string(endLine));
  }

  auto res = runPipeCommand(args, allText, bufferWorkDir());
  if (!res)
    return;
  if (res->killed) {
    setStatus("Format killed");
    return;
  }
  if (res->exitCode != 0) {
    auto& err = res->stderrData;
    auto nl = err.find('\n');
    setStatus(nl != std::string::npos ? err.substr(0, nl) : err);
    return;
  }
  if (res->stdoutData == allText) {
    setStatus("Already formatted");
    return;
  }

  auto savedLine = _buf->point().line;
  auto savedOffset = _buf->point().offset;
  _buf->clearMark();
  _buf->deleteRegion({0, 0}, endPt);
  _buf->insertText(res->stdoutData);
  auto line = std::min(savedLine, _buf->lineCount() - 1);
  auto offset = std::min(savedOffset, _buf->lineContent(line).size());
  _buf->setPoint(line, offset);
  auto& [rows, cols] = _size;
  _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
  setStatus("Formatted");
}

void Screen::openPipeReplacePrompt() {
  _prompt = Prompt{
      .label = "Shell command: ",
      .onResult = [this](std::string const& value) { executePipeReplace(value); },
  };
}

void Screen::executePipeReplace(std::string command) {
  if (!_buf || !_buf->mark())
    return;

  auto from = *_buf->mark();
  auto to = _buf->point();
  auto regionText = _buf->extractRegion(from, to);

  auto res = runPipeCommand({"/bin/sh", "-c", std::move(command)}, regionText, bufferWorkDir());

  if (!res || res->killed) {
    setStatus(res ? "Command killed" : "Command failed to start");
    return;
  }

  _buf->deleteRegion(from, to);
  _buf->clearMark();

  if (!res->stdoutData.empty()) {
    _buf->insertText(res->stdoutData);
    if (!res->stderrData.empty()) {
      std::string trimmed;
      trimmed.reserve(res->stderrData.size());
      for (char c : res->stderrData)
        if (c != '\n' && c != '\r')
          trimmed += c;
      setStatus("Command returned error output: " + trimmed);
    }
  } else if (!res->stderrData.empty()) {
    _buf->insertText("STDERR:\n" + res->stderrData);
  }

  auto& [rows, cols] = _size;
  _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
}

bool Screen::handleEscapeInput(std::optional<char>& pendingChar) {
  auto& [rows, cols] = _size;
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 50) > 0) {
    // Escape sequence: read the next bytes to identify the key.
    char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == ansi::CsiIntro) {
      if (read(STDIN_FILENO, &seq[1], 1) == 1) {
        switch (seq[1]) {
        case ansi::UpArrow:
          if (_buf) {
            if (_wrap)
              _buf->moveUp(cols);
            else
              _buf->movePhysicalUp();
            if (_buf->point() < _buf->viewFrom())
              _buf->scrollUp(_wrap);
          }
          break;
        case ansi::DownArrow:
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
        case ansi::RightArrow:
          if (_buf)
            _buf->moveRight();
          break;
        case ansi::LeftArrow:
          if (_buf)
            _buf->moveLeft();
          break;
        case ansi::Home:
          if (_buf)
            _buf->moveHome();
          break;
        case ansi::End:
          if (_buf)
            _buf->moveEnd();
          break;
        case ansi::ModifiedKey: {
          // Modified key: ESC[1;modX (e.g. Ctrl+Arrow = ESC[1;5X)
          char semi, mod, dir;
          if (read(STDIN_FILENO, &semi, 1) == 1 && semi == ansi::Separator &&
              read(STDIN_FILENO, &mod, 1) == 1 && read(STDIN_FILENO, &dir, 1) == 1) {
            if (mod == ansi::ModCtrl && _buf) {
              switch (dir) {
              case ansi::RightArrow:
                _buf->moveWordRight();
                break;
              case ansi::LeftArrow:
                _buf->moveWordLeft();
                break;
              case ansi::UpArrow:
                if (_wrap && _buf->currentLineWraps(cols))
                  _buf->movePhysicalUp();
                else
                  _buf->moveParagraphUp();
                break;
              case ansi::DownArrow:
                if (_wrap && _buf->currentLineWraps(cols))
                  _buf->movePhysicalDown();
                else
                  _buf->moveParagraphDown();
                break;
              case ansi::Home:
                _buf->setPoint(0, 0);
                break;
              case ansi::End:
                _buf->setPoint(_buf->lineCount() - 1,
                               _buf->lineContent(_buf->lineCount() - 1).size());
                break;
              }
              _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
            }
          }
          break;
        }
        case ansi::Delete: {
          char next;
          if (read(STDIN_FILENO, &next, 1) == 1) {
            if (next == ansi::Terminator) {
              // Plain Delete: ESC[3~
              if (_buf && _buf->mark()) {
                _buf->deleteRegion(_buf->mark().value(), _buf->point());
                _pasteState.reset();
              } else if (_buf) {
                _buf->deleteAtPoint();
                _pasteState.reset();
              }
            } else if (next == ansi::Separator) {
              // Modified Delete: ESC[3;mod~
              char mod, tilde;
              if (read(STDIN_FILENO, &mod, 1) == 1 && read(STDIN_FILENO, &tilde, 1) == 1 &&
                  tilde == ansi::Terminator) {
                if (mod == ansi::ModCtrl && _buf)
                  _buf->deleteChunkAtPoint();
              }
            }
          }
          break;
        }
        case ansi::PageUp: {
          char tilde;
          if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == ansi::Terminator && _buf)
            _buf->pageUp(rowsForBuffer(), cols, _wrap);
          break;
        }
        case ansi::PageDown: {
          char tilde;
          if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == ansi::Terminator && _buf)
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
    } else if (!handleEscChord(seq[0]) && seq[0] != ansi::CsiIntro) {
      // Not a CSI sequence or known chord — insert the byte verbatim
      // (e.g. Alt+key sends ESC followed by the key).
      if (_buf && static_cast<unsigned char>(seq[0]) >= key::PrintableMin)
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
      if (next == key::Escape) {
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
  if (c == key::Enter || c == key::Newline) {
    // Buffer list: Enter selects the highlighted buffer.
    auto* blb = dynamic_cast<BufferListBuffer*>(_buf);
    if (blb && _prevBuf && _bufMgr) {
      auto name = blb->selectedName();
      Buffer* target = _bufMgr->find(name);
      if (target) {
        _prevBuf = nullptr;
        setBuffer(*target);
      }
    } else if (auto* db = dynamic_cast<DirBuffer*>(_buf); db && _bufMgr) {
      // Directory buffer: Enter opens file or navigates into directory.
      if (db->selectedIsDir()) {
        setBuffer(_bufMgr->openDir(db->selectedPath()));
      } else {
        setBuffer(_bufMgr->open(db->selectedPath()));
      }
    } else if (_buf) {
      _buf->insert('\n');
      _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
      _pasteState.reset();
    }
  } else if (c == key::Backspace) {
    // Backspace: if mark active, delete region (no paste ring push).
    if (_buf && _buf->mark()) {
      _buf->deleteRegion(_buf->mark().value(), _buf->point());
      _pasteState.reset();
    } else if (_buf) {
      _buf->deleteBeforePoint();
      _pasteState.reset();
    }
  } else if (c == key::Escape) {
    return handleEscapeInput(pendingChar);
  } else if (c == key::Tab) {
    if (_buf && !_buf->readonly()) {
      if (_buf->mode() == BufferMode::Cpp) {
        if (!_buf->hasPath()) {
          setStatus("Save file first to determine format");
        } else {
          auto line = _buf->point().line + 1;
          executeFormat({{line, line}});
          _buf->adjustCursorToIndent();
        }
      } else {
        _buf->insert('\t');
      }
      _buf->ensureVisible(rowsForBuffer(), cols, _wrap);
      _pasteState.reset();
    }
  } else if (static_cast<unsigned char>(c) < key::PrintableMin) {
    auto s = shortcutFromCtrlChar(c);
    if (auto action = handleShortcut(s))
      dispatchAction(*action);
    else
      setStatus(formatShortcut(s) + " not recognised.");
  } else {
    // '?' on a bracket in C mode → jump to matching bracket.
    if (c == '?' && _buf && _buf->mode() == BufferMode::Cpp && !_buf->readonly() &&
        _buf->gotoMatchingBracket()) {
      // Jump succeeded — nothing else to do.
    } else if (auto* db = dynamic_cast<DirBuffer*>(_buf)) {
      // Directory buffer keys.
      if (c == 'h') {
        db->toggleHidden();
      } else if (c == 'd') {
        db->toggleDeleteMark();
        if (_buf->point().line + 1 < _buf->lineCount())
          _buf->movePhysicalDown();
      } else if (c == 'x') {
        auto marked = db->markedPaths();
        if (marked.empty()) {
          setStatus("No items marked for deletion.");
        } else {
          openConfirmPrompt(
              "Delete " + std::to_string(marked.size()) + " item(s)? y/n: ",
              [this](std::string const& answer) {
                if (answer == "y") {
                  auto* db2 = dynamic_cast<DirBuffer*>(_buf);
                  if (db2) {
                    auto [count, err] = db2->executeDelete();
                    if (err.empty())
                      setStatus("Deleted " + std::to_string(count) + " item(s).");
                    else
                      setStatus("Deleted " + std::to_string(count) + "; errors: " + err);
                  }
                } else {
                  _statusMsg.clear();
                  _statusExpiry.reset();
                }
              },
              [this]() {
                _statusMsg.clear();
                _statusExpiry.reset();
              });
        }
      }
    } else if (_buf && _buf->readonly() && c == 'q' && _prevBuf) {
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
      std::string question;
      if (!buf->hasPath())
        question = "Exiting. Save temporary buffer? y/n/Esc: ";
      else
        question = "Exiting. Save " + _bufMgr->displayName(buf->name()) + "? y/n/Esc: ";
      openConfirmKillPrompt(std::move(question));
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
static std::optional<std::size_t> matchAt(std::string const& line, std::size_t off,
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
static std::optional<std::pair<std::size_t, std::size_t>> findFirstMatch(std::string const& line,
                                                                         std::size_t from,
                                                                         std::string const& pattern,
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
findLastMatch(std::string const& line, std::size_t from, std::size_t limit,
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
    openGoToLinePrompt();
    break;
  case Action::SwitchBuffer:
    _prevBuf = nullptr;
    openSwitchBufferPrompt();
    break;
  case Action::KillBuffer:
    if (_buf && _bufMgr && (dynamic_cast<DirBuffer*>(_buf) || !_buf->readonly())) {
      if (!_buf->modified()) {
        auto name = _buf->name();
        if (_prevBuf == _buf)
          _prevBuf = nullptr;
        setBuffer(_bufMgr->kill(name));
      } else {
        openConfirmKillPrompt(!_buf->hasPath()
                                  ? "Killing temporary buffer. Save to file first? y/n: "
                                  : "Killing modified buffer. Save to file first? y/n: ");
      }
    }
    break;
  case Action::Save:
    if (_buf && !_buf->readonly()) {
      if (_buf->hasPath()) {
        if (!std::filesystem::exists(_buf->name())) {
          openConfirmPrompt(
              "File " + _buf->name() + " doesn't exist. Really save there? y/n: ",
              [this](std::string const& answer) {
                if (answer == "y")
                  saveAndMaybeKill();
                else {
                  _killPending = false;
                  _exitPending = false;
                }
              },
              [this]() {
                _killPending = false;
                _exitPending = false;
              });
        } else {
          auto res = _buf->save();
          if (res)
            setStatus("Saved to " + _buf->name());
          else
            setStatus(res.error());
        }
      } else {
        openSaveAsPrompt();
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
    openFindPrompt(false);
    break;
  case Action::FindBackward:
    openFindPrompt(true);
    break;
  case Action::Revert:
    if (_buf && !_buf->readonly()) {
      if (!_buf->hasPath()) {
        setStatus("Can't revert from file in temporary buffers.");
      } else {
        openConfirmPrompt(
            "Really revert buffer from file " + _buf->name() + "? y/n: ",
            [this](std::string const& answer) {
              if (answer == "y") {
                if (_buf) {
                  auto res = _buf->revert();
                  if (res)
                    setStatus("Reverted from " + _buf->name());
                  else
                    setStatus(res.error());
                }
              }
            },
            [] {});
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
  case Action::Format:
    if (_buf) {
      std::optional<std::pair<std::size_t, std::size_t>> range;
      if (_buf->mark()) {
        auto markLine = _buf->mark()->line;
        auto pointLine = _buf->point().line;
        range = {std::min(markLine, pointLine) + 1, std::max(markLine, pointLine) + 1};
      }
      executeFormat(range);
    }
    break;
  case Action::PipeReplace:
    if (_buf && !_buf->readonly()) {
      if (!_buf->mark())
        setStatus("No mark set");
      else
        openPipeReplacePrompt();
    }
    break;
  case Action::SetMode:
    if (_buf && !_buf->readonly())
      openSetModePrompt();
    break;
  case Action::DirBrowser:
    if (_bufMgr)
      setBuffer(_bufMgr->openDir(std::filesystem::current_path()));
    break;
  }
}
