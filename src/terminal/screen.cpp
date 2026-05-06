#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_manager.hpp"
#include "buffer/text_buffer.hpp"
#include "byte_constants.hpp"
#include "error_log.hpp"
#include "replace.hpp"
#include "search.hpp"
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
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>

// Read a full UTF-8 character from stdin (given the lead byte) and insert
// it into the buffer as a single codepoint.
static void insertCharWithUtf8(BufferView& view, char c) {
  view.insert(readUtf8Codepoint(static_cast<unsigned char>(c)));
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

// Set by the SIGWINCH handler from signal context; read and cleared by the
// main thread in loop(), handleEscapeInput()/handlePromptInput() (via
// readByte), and runPipeCommand(). Single-consumer at any moment because
// only one of those code paths runs at a time on the main thread.
static sig_atomic_t volatile g_resized = 0;

// Build a PromptDisplay summarising the current completions list,
// shared by openSaveAsPrompt and openSwitchBufferPrompt: dim-styled
// hint after the input, optional [a/b/c] for a small number of
// alternatives, or an "(N matches)" suffix when there are too many.
// Caller is responsible for tracking the user's selected index in
// `idxSelected` so we can suppress the hint mid-tab-cycle.
static PromptDisplay
buildCompletionDisplay(std::string const& input,
                       std::vector<std::pair<std::string, std::string>> const& completions,
                       bool idxSelected) {
  if (idxSelected || completions.empty())
    return {"", 0};
  // Escape after stripping the typed prefix: `match.starts_with(input)`
  // is computed on the raw byte string the user actually typed, while
  // `hint` is what we paint to the terminal — so that gets escaped.
  auto hintFor = [&](std::string const& match) -> std::string {
    if (match.starts_with(input))
      return escapeNonPrintable(match.substr(input.size()));
    return escapeNonPrintable(match);
  };
  auto hint = hintFor(completions[0].first);
  if (completions.size() == 1) {
    if (hint.empty())
      return {"", 0};
    return {std::string(ansi::Dim) + hint + std::string(ansi::DimOff), visualWidth(hint)};
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
        h = escapeNonPrintable(completions[i].first);
      rendered += h;
      vw += visualWidth(h);
    }
    rendered += std::string("]") + std::string(ansi::DimOff);
    vw++;
    return {std::move(rendered), vw};
  }
  auto cnt = " (" + std::to_string(completions.size()) + " matches)";
  return {std::string(ansi::Dim) + hint + cnt + std::string(ansi::DimOff),
          visualWidth(hint) + cnt.size()};
}

// Tunable time constants. Grouped here so the inline numbers below
// describe a tunable rather than a magic number.
namespace {
// Time after which a status message ages out of the modeline.
constexpr auto kStatusMessageTtl = std::chrono::seconds(10);
// Time after which a Find highlight ages out and is cleared.
constexpr auto kHighlightTtl = std::chrono::seconds(3);
// Time the kill-prompt waits before showing "Kill? y/n" for a long
// pipe command (and the same value used to refresh the deadline if
// the user answers no).
constexpr auto kKillPromptDelay = std::chrono::seconds(10);
// Initial poll timeout when distinguishing bare-Esc from the start of
// an escape sequence on the input stream.
constexpr int kEscDisambiguationMs = 50;
} // namespace

static void handle_sigwinch(int) { g_resized = 1; }

// Read a single byte from stdin, retrying on EINTR. Returns true on a
// successful 1-byte read, false on EOF or non-EINTR error. Used by the
// escape-sequence and prompt input paths so a SIGWINCH mid-CSI doesn't
// drop the next byte and corrupt arrow-key dispatch.
static bool readByte(char& out) {
  for (;;) {
    ssize_t n = read(STDIN_FILENO, &out, 1);
    if (n == 1)
      return true;
    if (n == -1 && errno == EINTR)
      continue;
    return false;
  }
}

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
  // so the loop can detect the resize promptly. POSIX leaves sigset_t
  // opaque, so we cannot rely on value-init producing an empty mask.
  struct sigaction sa = {};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = handle_sigwinch;
  sigaction(SIGWINCH, &sa, nullptr);

  // Pipe-based formatters and similar tools may close their stdin
  // before reading all our input (e.g. `head -n 1`). Default SIGPIPE
  // disposition would terminate the editor outright, skipping the
  // SetupTerminal destructor and leaving the user's terminal in raw
  // mode. Ignoring lets `write` return EPIPE so we can recover.
  signal(SIGPIPE, SIG_IGN);

  // Set the terminal title.
  auto cwd = std::filesystem::current_path().string();
  std::string title = "\x1b]0;cx@" + cwd + "\x07";
  writeAll(STDOUT_FILENO, title.data(), title.size());

  draw();
}

void Screen::setBuffer(Buffer& buf) {
  auto& [rows, cols] = _size;
  if (!_layout) {
    _layout = std::make_unique<PanelLayout>(buf, rows.value, cols.value);
  } else {
    auto& panel = _layout->focused();
    // Exit history mode on the old buffer before switching away.
    if (_view && _view->inHistoryMode())
      _view->exitHistoryModeCommit();
    // Save view state if this is the last view for the old buffer.
    if (_buf && _layout->viewCountFor(*_buf) == 1)
      panel.saveViewState();
    bool restoreState = (_layout->viewCountFor(buf) == 0);
    panel.setBuffer(buf, restoreState);
  }
  updateFocusPointers();
  _pasteState.reset();
  if (_bufMgr && (!buf.readonly() || dynamic_cast<DirBuffer*>(&buf)))
    _bufMgr->touch(buf.name());
  checkLineEndings(buf);
  draw();
}

void Screen::checkLineEndings(Buffer& buf) {
  auto* text = dynamic_cast<TextBuffer*>(&buf);
  if (!text)
    return;
  auto style = text->lineEndingStyle();
  if (style == LineEndingStyle::Unix)
    return;
  // Only ever prompt or alert once per load, regardless of outcome.
  text->acknowledgeLineEndingStyle();
  if (style == LineEndingStyle::Mixed) {
    setStatus("Mixed line endings detected — not translating");
    return;
  }
  // If another prompt is already up (e.g. setBuffer called mid-confirm)
  // we cannot open a second one — fall back to a status message.
  if (_prompt) {
    setStatus(style == LineEndingStyle::Crlf ? "CRLF line endings detected"
                                             : "CR line endings detected");
    return;
  }
  std::string kind = (style == LineEndingStyle::Crlf) ? "'\\r\\n'" : "'\\r'";
  // Capture the buffer name rather than a pointer: by the time the
  // prompt completes the buffer may have been killed.
  std::string name = buf.name();
  openConfirmPrompt(
      "Non-Unix file ending " + kind + " detected. Translate to '\\n' throughout? y/n: ",
      [this, name](std::string const& answer) {
        if (answer != "y" || !_bufMgr)
          return;
        if (auto* b = _bufMgr->find(name)) {
          if (auto* t = dynamic_cast<TextBuffer*>(b))
            t->normalizeLineEndings();
        }
      },
      nullptr);
}

void Screen::updateFocusPointers() {
  if (_layout) {
    _buf = &_layout->focused().buffer();
    _view = &_layout->focused().view();
  } else {
    _buf = nullptr;
    _view = nullptr;
  }
}

void Screen::setBufferManager(BufferManager& mgr) { _bufMgr = &mgr; }

bool Screen::requireNotInHistoryMode() {
  if (_view && _view->inHistoryMode()) {
    setStatus("Exit history mode first (Ctrl-U).");
    return true;
  }
  return false;
}

void Screen::drawModeline(Panel& panel, bool focused) {
  auto& view = panel.view();
  auto& pbuf = panel.buffer();
  auto rect = panel.rect();
  auto pc = panel.totalCols();

  // Cursor position prefix.
  std::string status;
  std::size_t status_vwidth = 0;
  {
    auto [line, col] = view.lineCol();
    status = std::to_string(line) + ":" + std::to_string(col);
    status_vwidth = visualWidth(status);
  }

  // History-mode badge.
  if (view.inHistoryMode()) {
    if (auto* tb = dynamic_cast<TextBuffer*>(&pbuf); tb && !tb->history().empty()) {
      auto idx = *view.historyIndex();
      auto total = tb->history().size();
      auto t = std::chrono::system_clock::to_time_t(tb->history().at(idx).timestamp);
      std::tm tm{};
      localtime_r(&t, &tm);
      char tbuf[32];
      std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
      std::string hseg = " | v " + std::to_string(idx + 1) + "/" + std::to_string(total) + "  " +
                         tbuf + "  [/] step  Ctrl-U commit";
      status += hseg;
      status_vwidth = visualWidth(status);
    }
  }

  // Prompt and status messages only on the focused panel.
  if (focused) {
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
  }

  // Right-align: buffer name + mode + modification indicator.
  std::string filename =
      escapeNonPrintable(_bufMgr ? _bufMgr->displayName(pbuf.name()) : pbuf.name());
  std::string suffix;
  if (!pbuf.readonly())
    suffix += pbuf.mode() == BufferMode::Cpp   ? " [c]"
              : pbuf.mode() == BufferMode::Md  ? " [md]"
              : pbuf.mode() == BufferMode::Jsn ? " [jsn]"
              : pbuf.mode() == BufferMode::Dif ? " [dif]"
                                               : " [txt]";
  suffix += pbuf.modified() ? " +" : "  ";
  auto suffix_vwidth = visualWidth(suffix);
  auto filename_vwidth = visualWidth(filename);
  auto buf_name_vwidth = filename_vwidth + suffix_vwidth;

  if (status_vwidth + buf_name_vwidth + 2 <= pc.value) {
    // Everything fits: pad between status and the buffer name block.
    status.append(pc.value - buf_name_vwidth - status_vwidth, ' ');
    status += filename + suffix;
  } else if (pc.value >= status_vwidth + 2 + suffix_vwidth + 1) {
    // Truncate the filename from the front to fit, preserving the
    // suffix and at least one column for the ellipsis.
    std::size_t filenameAvail = pc.value - status_vwidth - 2 - suffix_vwidth;
    std::string truncated = truncateVisualFromFront(filename, filenameAvail);
    std::size_t used = status_vwidth + visualWidth(truncated) + suffix_vwidth;
    if (used < pc.value)
      status.append(pc.value - used, ' ');
    status += truncated + suffix;
  } else if (status_vwidth < pc.value) {
    // No room for a meaningful buffer name — pad the rest with spaces.
    status.append(pc.value - status_vwidth, ' ');
  } else if (status_vwidth > pc.value) {
    // Status itself overflows (e.g. very long prompt); truncate from
    // the back so we never spill past the panel.
    std::size_t keep = 0;
    std::size_t w = 0;
    while (keep < status.size()) {
      auto info = charInfoAt(status.data() + keep, status.size() - keep);
      if (w + info.width > pc.value)
        break;
      w += info.width;
      keep += info.len;
    }
    status.resize(keep);
  }

  std::size_t modelineRow = rect.row + rect.rows - 1;
  ansi::appendCursorTo(_renderBuf, modelineRow, rect.col);
  if (focused)
    _renderBuf += ansi::Bold;
  else
    _renderBuf += ansi::Dim;
  _renderBuf += ansi::ReverseVideo;
  _renderBuf += status;
  _renderBuf += ansi::Reset;
}

void Screen::placePanelId(Panel const& panel, bool multiPanel) {
  if (!multiPanel || panel.id() < 1 || panel.id() > 9)
    return;
  auto rect = panel.rect();
  ansi::appendCursorTo(_renderBuf, rect.row, rect.col + rect.cols - 1);
  _renderBuf += ansi::Dim;
  _renderBuf += static_cast<char>('0' + panel.id());
  _renderBuf += ansi::DimOff;
}

void Screen::draw() {
  auto& [rows, cols] = _size;

  _renderBuf.clear();
  // 2× per cell underestimates a syntax-highlighted, reverse-video, or
  // diff-line-background frame, where SGR escape sequences plus the
  // per-row cursorTo can easily push real output past 8× cell count.
  // _renderBuf.capacity() persists across frames, so this only matters
  // on the first draw and after resizes — bias toward over-reservation
  // so the steady-state path never reallocates.
  std::size_t estimate = rows.value * cols.value * 8;
  if (_renderBuf.capacity() < estimate)
    _renderBuf.reserve(estimate);

  // Expire timed status message.
  if (_statusExpiry && std::chrono::steady_clock::now() >= *_statusExpiry) {
    _statusMsg.clear();
    _statusExpiry.reset();
  }

  // Expire find highlight.
  if (_highlightExpiry && std::chrono::steady_clock::now() >= *_highlightExpiry) {
    if (_view)
      _view->clearHighlight();
    _highlightExpiry.reset();
  }

  RowIdx cur_row{0};
  ColIdx cur_col{0};

  if (_layout) {
    bool multiPanel = _layout->count() > 1;
    int focusedId = _layout->focused().id();

    _layout->forEachPanel([&](Panel& panel) {
      auto& view = panel.view();
      auto rect = panel.rect();
      auto pr = panel.contentRows();
      auto pc = panel.totalCols();
      bool focused = (panel.id() == focusedId);

      // Clamp stale cursors in non-focused panels sharing a buffer.
      if (multiPanel && !focused)
        view.clampToBuffer();

      view.ensureVisible(pr, pc, _wrap);
      auto [cr, cc] = view.render(_renderBuf, pr, pc,
                                  {.showWhitespace = _showWhitespace,
                                   .wrap = _wrap,
                                   .focused = focused,
                                   .rowOffset = rect.row,
                                   .colOffset = rect.col});

      if (focused) {
        cur_row = RowIdx{rect.row + cr.value};
        cur_col = ColIdx{rect.col + cc.value};
      }

      drawModeline(panel, focused);
      placePanelId(panel, multiPanel);
    });
  }

  ansi::appendCursorTo(_renderBuf, cur_row.value, cur_col.value);
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
      if (n == 0)
        break; // clean EOF — stdin closed, exit loop
      if (n < 0) {
        // Transient error (e.g. EIO if the controlling tty is severed,
        // or EAGAIN if stdin was somehow flipped to non-blocking). Log
        // and retry rather than treating it as EOF; if it's persistent
        // the next read will fail again.
        logError(std::string("stdin read: ") + std::strerror(errno));
        continue;
      }

      // Also check after a successful read, in case the signal arrived
      // between loop iterations rather than during a blocked read().
      if (g_resized) {
        g_resized = 0;
        resize();
        draw();
      }
    }

    if (_prompt)
      handlePromptInput(c);
    else
      handleNormalInput(c, pendingChar);

    // Process pending exit: walk modified buffers or break.
    if (_exitPending && !_prompt) {
      if (tryExit())
        break;
    }

    syncPrimarySelection();
    draw();
  }
}

void Screen::syncPrimarySelection() {
  if (!_buf || !_view)
    return;
  auto mark = _view->mark();
  if (!mark)
    return; // No active selection — leave PRIMARY as-is (X11 convention).
  auto point = _view->point();
  if (*mark == point)
    return; // Empty region.
  _clipboard.setPrimary(_view->extractRegion(*mark, point));
}

void Screen::handlePromptInput(char c) {
  if (c == key::Enter || c == key::Newline) {
    if (!_prompt->showInput)
      return; // Enter is ignored on confirm prompts
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
    if (poll(&pfd, 1, kEscDisambiguationMs) > 0) {
      char seq[2];
      if (readByte(seq[0]) && seq[0] == ansi::CsiIntro && readByte(seq[1])) {
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
  } else if (c == key::Backspace) {
    // Remove one full UTF-8 code point (walk back over continuation bytes).
    // When input is already empty, swallow the byte — otherwise it would
    // fall into the printable branch below (Backspace is 0x7F, which is
    // >= PrintableMin) and silently end up in the prompt input.
    if (_prompt->input.empty())
      return;
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
    runAutocomplete();
  } else if (_prompt->onSpecialKey) {
    if (_prompt->onSpecialKey(c, _prompt->input))
      runAutocomplete();
  }
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
              _view->gotoLine(line, rowsForBuffer());
          },
  };
}

void Screen::openSaveAsPrompt() {
  struct CompletionState {
    std::vector<std::pair<std::string, std::string>> completions;
    std::optional<std::size_t> idx;
  };
  auto state = std::make_shared<CompletionState>();

  auto buildDisplay = [state](std::string const& input) -> PromptDisplay {
    return buildCompletionDisplay(input, state->completions, state->idx.has_value());
  };

  _prompt = Prompt{
      .label = "Save as: ",
      .autocomplete = [this, state,
                       buildDisplay](std::string const& input) -> Prompt::AutocompleteResult {
        if (_bufMgr)
          state->completions = _bufMgr->completions(input);
        state->idx.reset();
        return buildDisplay(input);
      },
      .onResult =
          [this, state](std::string const& value) {
            if (!_buf || !_bufMgr)
              return;
            // Resolve from tab completion or raw input.
            std::string resolved = value;
            if (state->idx && *state->idx < state->completions.size())
              resolved = state->completions[*state->idx].first;
            else if (state->completions.size() == 1 && !value.empty())
              resolved = state->completions[0].first;
            // Strip trailing slash — can't save as a directory.
            if (!resolved.empty() && resolved.back() == '/')
              resolved.pop_back();
            if (resolved.empty())
              return;
            if (std::filesystem::exists(resolved)) {
              // Defer rename until the user confirms the overwrite.
              auto savedName = _buf->name();
              openConfirmPrompt(
                  "Overwrite " + resolved + "? y/n: ",
                  [this, savedName, resolved](std::string const& answer) {
                    if (answer == "y") {
                      if (_buf && _bufMgr) {
                        _bufMgr->rename(savedName, resolved);
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
            _bufMgr->rename(_buf->name(), resolved);
            saveAndMaybeKill();
          },
      // Abort also cancels any in-progress exit flow, so the loop does not
      // re-enter tryExit() and bounce the user back to the same prompt.
      .onAbort =
          [this]() {
            _killPending = false;
            _exitPending = false;
          },
      .onTab = [this, state, buildDisplay](std::string& input) -> PromptDisplay {
        if (state->completions.empty())
          return {"", 0};
        if (!state->idx)
          state->idx = 0;
        else
          state->idx = (*state->idx + 1) % state->completions.size();
        input = state->completions[*state->idx].first;
        // Navigate into directories when the match is unambiguous.
        if (input.ends_with('/') && state->completions.size() == 1) {
          if (_bufMgr)
            state->completions = _bufMgr->completions(input);
          state->idx.reset();
        }
        return buildDisplay(input);
      },
      .onRightArrow = [this, state, buildDisplay](std::string& input) -> PromptDisplay {
        if (state->completions.empty())
          return {"", 0};
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
    return buildCompletionDisplay(input, state->completions, state->idx.has_value());
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
          return PromptDisplay{std::string(ansi::Dim) + _lastSearch + std::string(ansi::DimOff),
                               visualWidth(_lastSearch)};
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

void Screen::openReplacePrompt() {
  if (!_buf || _buf->readonly()) {
    setStatus("Buffer is read-only");
    return;
  }
  _prompt = Prompt{
      .label = "Replace: ",
      .onResult =
          [this](std::string const& pattern) {
            if (pattern.empty() || !_buf)
              return;
            _prompt = Prompt{
                .label = "Replace '" + pattern + "' with: ",
                .onResult =
                    [this, pattern](std::string const& replacement) {
                      replacePump(pattern, replacement, 0);
                    },
            };
          },
  };
}

void Screen::replacePump(std::string pattern, std::string replacement, std::size_t replaced) {
  if (!_buf || _buf->readonly())
    return;
  FindFlags f(pattern);
  auto m = findNextMatchInBuffer(*_buf, _view->point(), pattern, f);
  if (!m) {
    _view->clearHighlight();
    _highlightExpiry.reset();
    setStatus("Replaced " + std::to_string(replaced) + " occurrence(s)");
    return;
  }
  _view->setPoint(m->first.line, m->first.offset);
  _view->setHighlight(m->first, m->second);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
  _highlightExpiry = std::chrono::steady_clock::now() + kHighlightTtl;
  openReplaceConfirmPrompt(std::move(pattern), std::move(replacement), replaced, m->first,
                           m->second);
}

void Screen::openReplaceConfirmPrompt(std::string pattern, std::string replacement,
                                      std::size_t replaced, BufferPoint matchStart,
                                      BufferPoint matchEnd) {
  _prompt = Prompt{
      .label = "Replace this? (y=yes n=skip !=all q=quit): ",
      .showInput = false,
      .autocomplete = [](std::string const& input) -> Prompt::AutocompleteResult {
        if (input.empty())
          return PromptDisplay{"", 0};
        char c = input.back();
        if (c >= 'A' && c <= 'Z')
          c = static_cast<char>(c + 32);
        if (c == 'y' || c == 'n' || c == '!' || c == 'q')
          return std::string{c};
        return PromptDisplay{"", 0};
      },
      .onResult =
          [this, pattern, replacement, replaced, matchStart, matchEnd](std::string const& answer) {
            if (!_buf || _buf->readonly())
              return;
            char c = answer.empty() ? 'q' : answer[0];
            if (c == 'q') {
              _view->clearHighlight();
              _highlightExpiry.reset();
              setStatus("Replaced " + std::to_string(replaced) + " occurrence(s)");
              return;
            }
            std::size_t newReplaced = replaced;
            if (c == 'y' || c == '!') {
              _view->deleteRegion(matchStart, matchEnd);
              auto after = _view->insertText(replacement);
              _view->setPoint(after.line, after.offset);
              ++newReplaced;
            } else {
              // 'n': advance past the match without modifying.
              _view->setPoint(matchEnd.line, matchEnd.offset);
            }
            if (c == '!') {
              auto extra = replaceAllForward(*_buf, _view->point(), pattern, replacement);
              newReplaced += extra;
              // setPoint via clampToBuffer: replaceAllForward mutated
              // the buffer behind _view's back, so its docVersion is stale.
              _view->clampToBuffer();
              _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
              _view->clearHighlight();
              _highlightExpiry.reset();
              setStatus("Replaced " + std::to_string(newReplaced) + " occurrence(s)");
              return;
            }
            replacePump(pattern, replacement, newReplaced);
          },
      .onAbort =
          [this, replaced]() {
            _view->clearHighlight();
            _highlightExpiry.reset();
            setStatus("Replaced " + std::to_string(replaced) + " occurrence(s)");
          },
  };
}

void Screen::openSetModePrompt() {
  _prompt = Prompt{
      .label = "Mode [txt/c/md/jsn/dif]: ",
      .onResult =
          [this](std::string const& value) {
            if (value == "c" || value == "cpp" || value == "c++") {
              if (_buf)
                _buf->setMode(BufferMode::Cpp);
              setStatus("Mode: c");
            } else if (value == "jsn" || value == "json") {
              if (_buf)
                _buf->setMode(BufferMode::Jsn);
              setStatus("Mode: jsn");
            } else if (value == "md" || value == "markdown") {
              if (_buf)
                _buf->setMode(BufferMode::Md);
              setStatus("Mode: md");
            } else if (value == "txt" || value == "text") {
              if (_buf)
                _buf->setMode(BufferMode::Txt);
              setStatus("Mode: txt");
            } else if (value == "dif" || value == "diff" || value == "patch") {
              if (_buf)
                _buf->setMode(BufferMode::Dif);
              setStatus("Mode: dif");
            } else {
              setStatus("Unknown mode: " + value + " (use 'txt', 'c', 'md', 'jsn', or 'dif')");
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

void Screen::killBufferAndSwitchPanels(std::string const& name) {
  if (!_bufMgr || !_layout)
    return;
  // Collect non-focused panel IDs that show this buffer or have it as prevBuf.
  Buffer* killed = _buf;
  std::vector<int> affectedIds;
  int focusedId = _layout->focused().id();
  _layout->forEachPanel([&](Panel& panel) {
    if (panel.id() == focusedId)
      return;
    if (panel.prevBuf == killed)
      panel.prevBuf = nullptr;
    if (&panel.buffer() == killed)
      affectedIds.push_back(panel.id());
  });
  // Destroy the buffer and get its replacement.
  auto& replacement = _bufMgr->kill(name);
  // Switch affected panels to the replacement.
  for (int id : affectedIds) {
    if (auto* panel = _layout->byId(id))
      panel->setBuffer(replacement, true);
  }
  // Switch focused panel.
  setBuffer(replacement);
}

void Screen::saveAndMaybeKill() {
  if (!_buf)
    return;
  auto res = _buf->save();
  if (res) {
    setStatus("Saved to " + escapeNonPrintable(_buf->name()));
    if (_killPending && _bufMgr) {
      _killPending = false;
      auto name = _buf->name();
      if (prevBuf() == _buf)
        prevBuf() = nullptr;
      killBufferAndSwitchPanels(name);
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
                auto name = _buf->name();
                if (prevBuf() == _buf)
                  prevBuf() = nullptr;
                killBufferAndSwitchPanels(name);
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
            if (prevBuf() == _buf)
              prevBuf() = nullptr;
            killBufferAndSwitchPanels(name);
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
    // Reset SIGWINCH to default — execvp() will reset it anyway, but if
    // we fail before exec the child should not run our handler against
    // its (separate) copy of g_resized.
    signal(SIGWINCH, SIG_DFL);
    // Restore default SIGPIPE so the child sees the conventional
    // shell-style behaviour rather than our parent-only ignore.
    signal(SIGPIPE, SIG_DFL);
    if (!workDir.empty()) {
      if (chdir(workDir.c_str()) < 0) {
        auto msg = std::string("chdir(") + workDir + "): " + std::strerror(errno) + "\n";
        writeAll(pipeErr[1], msg.data(), msg.size());
        _exit(126);
      }
    }
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
    writeAll(STDERR_FILENO, msg.data(), msg.size());
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
  auto deadline = std::chrono::steady_clock::now() + kKillPromptDelay;

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
              pid_t wp;
              while ((wp = waitpid(pid, &wstatus, WNOHANG)) < 0 && errno == EINTR) {
              }
              if (wp > 0)
                return killReturn();
              pollfd drain[] = {{pipeOut[0], POLLIN, 0}, {pipeErr[0], POLLIN, 0}};
              int pr = poll(drain, 2, 100);
              if (pr < 0 && errno == EINTR) {
                // SIGWINCH (or other) interrupted — surface a redraw and
                // retry without trusting stale revents.
                if (g_resized) {
                  g_resized = 0;
                  resize();
                  draw();
                }
                continue;
              }
              if (pr <= 0)
                continue;
              bool dummy = false;
              readPipe(pipeOut[0], drain[0].revents, stdoutData, dummy);
              readPipe(pipeErr[0], drain[1].revents, stderrData, dummy);
            }
            kill(pid, SIGKILL);
            {
              int wstatus;
              while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
              }
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
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
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
  auto allText = _view->extractRegion({0, 0}, endPt);

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

  auto savedLine = _view->point().line;
  auto savedOffset = _view->point().offset;
  _view->clearMark();
  _view->deleteRegion({0, 0}, endPt);
  _view->insertText(res->stdoutData);
  auto line = std::min(savedLine, _buf->lineCount() - 1);
  auto offset = std::min(savedOffset, _buf->lineContent(line).size());
  _view->setPoint(line, offset);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
  setStatus("Formatted");
}

void Screen::openPipeReplacePrompt() {
  _prompt = Prompt{
      .label = "Shell command: ",
      .onResult = [this](std::string const& value) { executePipeReplace(value); },
  };
}

void Screen::executePipeReplace(std::string command) {
  if (!_buf || !_view->mark())
    return;

  auto from = *_view->mark();
  auto to = _view->point();
  auto regionText = _view->extractRegion(from, to);

  auto res = runPipeCommand({"/bin/sh", "-c", std::move(command)}, regionText, bufferWorkDir());

  if (!res || res->killed) {
    setStatus(res ? "Command killed" : "Command failed to start");
    return;
  }

  _view->deleteRegion(from, to);
  _view->clearMark();

  if (!res->stdoutData.empty()) {
    _view->insertText(res->stdoutData);
    if (!res->stderrData.empty()) {
      std::string trimmed;
      trimmed.reserve(res->stderrData.size());
      for (char c : res->stderrData)
        if (c != '\n' && c != '\r')
          trimmed += c;
      setStatus("Command returned error output: " + trimmed);
    }
  } else if (!res->stderrData.empty()) {
    _view->insertText("STDERR:\n" + res->stderrData);
  }

  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
}

void Screen::handleEscapeInput(std::optional<char>& pendingChar) {
  auto pcols = colsForBuffer();
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 50) > 0) {
    // Escape sequence: read the next bytes to identify the key.
    char seq[2];
    if (readByte(seq[0]) && seq[0] == ansi::CsiIntro) {
      if (readByte(seq[1])) {
        switch (seq[1]) {
        case ansi::UpArrow:
          if (_buf) {
            if (_wrap)
              _view->moveUp(pcols);
            else
              _view->movePhysicalUp();
            if (_view->point() < _view->viewFrom())
              _view->scrollUp(_wrap);
          }
          break;
        case ansi::DownArrow:
          if (_buf) {
            auto prev = _view->point();
            if (_wrap)
              _view->moveDown(pcols);
            else
              _view->movePhysicalDown();
            if (_view->point() == prev)
              _view->scrollViewNearEnd(rowsForBuffer());
            else
              _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
          }
          break;
        case ansi::RightArrow:
          if (_buf)
            _view->moveRight();
          break;
        case ansi::LeftArrow:
          if (_buf)
            _view->moveLeft();
          break;
        case ansi::Home:
          if (_buf)
            _view->moveHome();
          break;
        case ansi::End:
          if (_buf)
            _view->moveEnd();
          break;
        case ansi::ModifiedKey: {
          // Modified key: ESC[1;modX (e.g. Ctrl+Arrow = ESC[1;5X)
          char semi, mod, dir;
          if (readByte(semi) && semi == ansi::Separator && readByte(mod) && readByte(dir)) {
            if (mod == ansi::ModCtrl && _buf) {
              switch (dir) {
              case ansi::RightArrow:
                _view->moveWordRight();
                break;
              case ansi::LeftArrow:
                _view->moveWordLeft();
                break;
              case ansi::UpArrow:
                if (_wrap && _view->currentLineWraps(pcols))
                  _view->movePhysicalUp();
                else
                  _view->moveParagraphUp();
                break;
              case ansi::DownArrow:
                if (_wrap && _view->currentLineWraps(pcols))
                  _view->movePhysicalDown();
                else
                  _view->moveParagraphDown();
                break;
              case ansi::Home:
                _view->setPoint(0, 0);
                break;
              case ansi::End:
                _view->setPoint(_buf->lineCount() - 1,
                                _buf->lineContent(_buf->lineCount() - 1).size());
                break;
              }
              _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
            }
          }
          break;
        }
        case ansi::Delete: {
          char next;
          if (readByte(next)) {
            if (next == ansi::Terminator) {
              // Plain Delete: ESC[3~
              if (_buf && _view->mark()) {
                _view->deleteRegion(_view->mark().value(), _view->point());
                _pasteState.reset();
              } else if (_buf) {
                _view->deleteAtPoint();
                _pasteState.reset();
              }
            } else if (next == ansi::Separator) {
              // Modified Delete: ESC[3;mod~
              char mod, tilde;
              if (readByte(mod) && readByte(tilde) && tilde == ansi::Terminator) {
                if (mod == ansi::ModCtrl && _buf)
                  _view->deleteChunkAtPoint();
              }
            }
          }
          break;
        }
        case ansi::PageUp: {
          char tilde;
          if (readByte(tilde) && tilde == ansi::Terminator && _buf)
            _view->pageUp(rowsForBuffer(), pcols, _wrap);
          break;
        }
        case ansi::PageDown: {
          char tilde;
          if (readByte(tilde) && tilde == ansi::Terminator && _buf)
            _view->pageDown(rowsForBuffer(), pcols, _wrap);
          break;
        }
        default: {
          // Drain remaining bytes of unrecognised escape sequence.
          char discard;
          struct pollfd dp = {STDIN_FILENO, POLLIN, 0};
          while (poll(&dp, 1, 0) > 0)
            if (!readByte(discard))
              break;
          break;
        }
        }
      }
    } else if (!handleEscChord(seq[0]) && seq[0] != ansi::CsiIntro) {
      // Not a CSI sequence or known chord — insert the byte verbatim
      // (e.g. Alt+key sends ESC followed by the key).
      if (_buf && static_cast<unsigned char>(seq[0]) >= key::PrintableMin)
        insertCharWithUtf8(*_view, seq[0]);
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
        if (_buf && _buf->readonly() && prevBuf()) {
          setBuffer(*prevBuf());
          prevBuf() = nullptr;
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
}

void Screen::handleNormalInput(char c, std::optional<char>& pendingChar) {
  if (_view && _highlightExpiry) {
    _view->clearHighlight();
    _highlightExpiry.reset();
  }
  auto pcols = colsForBuffer();
  if (c == key::Enter || c == key::Newline) {
    // Buffer list: Enter selects the highlighted buffer.
    auto* blb = dynamic_cast<BufferListBuffer*>(_buf);
    if (blb && prevBuf() && _bufMgr) {
      auto name = blb->selectedName(_view->point());
      Buffer* target = _bufMgr->find(name);
      if (target) {
        prevBuf() = nullptr;
        setBuffer(*target);
      }
    } else if (auto* db = dynamic_cast<DirBuffer*>(_buf); db && _bufMgr) {
      // Directory buffer: Enter opens file or navigates into directory.
      auto pt = _view->point();
      if (db->selectedIsDir(pt)) {
        setBuffer(_bufMgr->openDir(db->selectedPath(pt)));
      } else {
        setBuffer(_bufMgr->open(db->selectedPath(pt)));
      }
    } else if (_buf) {
      _view->insert('\n');
      _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
      _pasteState.reset();
    }
  } else if (c == key::Backspace) {
    // Backspace: if mark active, delete region (no paste ring push).
    if (_buf && _view->mark()) {
      _view->deleteRegion(_view->mark().value(), _view->point());
      _pasteState.reset();
    } else if (_buf) {
      _view->deleteBeforePoint();
      _pasteState.reset();
    }
  } else if (c == key::Escape) {
    handleEscapeInput(pendingChar);
    return;
  } else if (c == key::Tab) {
    if (_buf && !_buf->readonly()) {
      if (_buf->mode() == BufferMode::Cpp) {
        if (!_buf->hasPath()) {
          setStatus("Save file first to determine format");
        } else {
          auto line = _view->point().line + 1;
          executeFormat({{line, line}});
          _view->adjustCursorToIndent();
        }
      } else {
        _view->insert('\t');
      }
      _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
      _pasteState.reset();
    }
  } else if (c == key::CtrlX) {
    // Ctrl-x prefix: read next key for two-key sequence.
    handleCtrlXInput();
    return;
  } else if (static_cast<unsigned char>(c) < key::PrintableMin) {
    auto s = shortcutFromCtrlChar(c);
    if (auto action = handleShortcut(s))
      dispatchAction(*action);
    else
      setStatus(formatShortcut(s) + " not recognised.");
  } else {
    // History mode: '[' and ']' step through versions; all other
    // printable chars are swallowed (edits are already no-ops via
    // readonly()).
    if (_view && _view->inHistoryMode()) {
      if (c == '[')
        _view->stepHistory(-1);
      else if (c == ']')
        _view->stepHistory(+1);
      if (_buf)
        _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
      return;
    }
    // '?' on a bracket in C/JSON mode → jump to matching bracket.
    if (c == '?' && _buf && (_buf->mode() == BufferMode::Cpp || _buf->mode() == BufferMode::Jsn) &&
        !_buf->readonly() && _view->gotoMatchingBracket()) {
      // Jump succeeded — nothing else to do.
    } else if (auto* db = dynamic_cast<DirBuffer*>(_buf)) {
      // Directory buffer keys.
      if (c == 'h') {
        auto hint = db->toggleHidden(_view->point());
        _view->setPoint(hint.point.line, hint.point.offset);
      } else if (c == 'd') {
        db->toggleDeleteMark(_view->point());
        if (_view->point().line + 1 < _buf->lineCount())
          _view->movePhysicalDown();
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
                    auto result = db2->executeDelete();
                    _view->setPoint(result.hint.point.line, result.hint.point.offset);
                    if (result.error.empty())
                      setStatus("Deleted " + std::to_string(result.count) + " item(s).");
                    else
                      setStatus("Deleted " + std::to_string(result.count) +
                                "; errors: " + result.error);
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
    } else if (_buf && _buf->readonly() && c == 'q' && prevBuf()) {
      setBuffer(*prevBuf());
      prevBuf() = nullptr;
    } else if (_buf) {
      insertCharWithUtf8(*_view, c);
      _pasteState.reset();
    }
  }
}

RowIdx Screen::rowsForBuffer() const {
  if (_layout)
    return _layout->focused().contentRows();
  auto& [rows, cols] = _size;
  return RowIdx{rows.value > 1 ? rows.value - 1 : 1};
}

ColIdx Screen::colsForBuffer() const {
  if (_layout)
    return _layout->focused().totalCols();
  return std::get<1>(_size);
}

Buffer*& Screen::prevBuf() { return _layout->focused().prevBuf; }

void Screen::resize() {
  struct winsize ws = {};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    _size = {RowIdx{ws.ws_row}, ColIdx{ws.ws_col}};
  if (_layout) {
    auto& [rows, cols] = _size;
    _layout->resize(rows.value, cols.value);
  }
}

void Screen::gotoLine(std::size_t line) {
  if (_view && line > 0)
    _view->gotoLine(line, rowsForBuffer());
}

void Screen::insertText(std::string_view text) {
  if (_view && !_buf->readonly())
    _view->insertText(text);
}

void Screen::setStatus(std::string msg) {
  _statusMsg = std::move(msg);
  _statusExpiry = std::chrono::steady_clock::now() + kStatusMessageTtl;
}

bool Screen::handleEscChord(char ch) {
  auto s = shortcutFromEscChar(ch);
  if (auto action = handleShortcut(s)) {
    dispatchAction(*action);
    return true;
  }
  return false;
}

void Screen::handleCtrlXInput() {
  setStatus("C-x ...");
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
      setStatus("C-x ...");
      draw();
      continue;
    }
    break;
  }

  if (gotNext) {
    auto s = shortcutFromCxChar(next);
    if (auto action = handleShortcut(s)) {
      dispatchAction(*action);
    } else {
      setStatus(formatShortcut(s) + " not recognised.");
    }
  }

  if (_statusMsg == "C-x ...") {
    _statusMsg.clear();
    _statusExpiry.reset();
  }
}

bool Screen::tryExit() {
  if (!_bufMgr)
    return true;

  // Dismiss any readonly overlay first.
  if (_buf && _buf->readonly() && prevBuf()) {
    setBuffer(*prevBuf());
    prevBuf() = nullptr;
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
        question = "Exiting. Save " + escapeNonPrintable(_bufMgr->displayName(buf->name())) +
                   "? y/n/Esc: ";
      openConfirmKillPrompt(std::move(question));
      return false;
    }
  }
  return true; // no modified buffers remain
}

void Screen::onFindMatch(std::size_t line, std::size_t matchStart, std::size_t matchEnd,
                         bool wrapped) {
  _view->setPoint(line, matchEnd);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
  _view->setHighlight({line, matchStart}, {line, matchEnd});
  _highlightExpiry = std::chrono::steady_clock::now() + kHighlightTtl;
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
  std::size_t startLine = _view->point().line;
  std::size_t startOff = _view->point().offset;
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

  _view->clearHighlight();
  _highlightExpiry.reset();
  setStatus("Not found");
}

void Screen::findPrev() {
  if (!_buf || _lastSearch.empty())
    return;
  FindFlags f(_lastSearch);

  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _view->point().line;
  std::size_t startOff = _view->point().offset;
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

  _view->clearHighlight();
  _highlightExpiry.reset();
  setStatus("Not found");
}

static bool isBlockTarget(std::string const& line, bool isDiff, bool hasConflicts) {
  if (isDiff && line.size() >= 2 && line[0] == '@' && line[1] == '@')
    return true;
  return hasConflicts && isConflictMarker(line);
}

void Screen::blockNext() {
  if (!_buf)
    return;
  bool isDiff = _buf->mode() == BufferMode::Dif;
  bool hasConflicts = _buf->hasConflicts();
  if (!isDiff && !hasConflicts) {
    setStatus("No blocks to navigate");
    return;
  }
  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _view->point().line;
  for (std::size_t count = 1; count < lineCount; ++count) {
    std::size_t li = (startLine + count) % lineCount;
    if (isBlockTarget(_buf->lineContent(li), isDiff, hasConflicts)) {
      _view->setPoint(li, 0);
      _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
      if (li <= startLine)
        setStatus("Block (wrapped)");
      return;
    }
  }
  setStatus("No blocks found");
}

void Screen::blockPrev() {
  if (!_buf)
    return;
  bool isDiff = _buf->mode() == BufferMode::Dif;
  bool hasConflicts = _buf->hasConflicts();
  if (!isDiff && !hasConflicts) {
    setStatus("No blocks to navigate");
    return;
  }
  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _view->point().line;
  for (std::size_t count = 1; count < lineCount; ++count) {
    std::size_t li = (startLine + lineCount - count) % lineCount;
    if (isBlockTarget(_buf->lineContent(li), isDiff, hasConflicts)) {
      _view->setPoint(li, 0);
      _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _wrap);
      if (li >= startLine)
        setStatus("Block (wrapped)");
      return;
    }
  }
  setStatus("No blocks found");
}

void Screen::dispatchAction(Action action) {
  if (action != Action::Find && action != Action::FindBackward && _buf) {
    _view->clearHighlight();
    _highlightExpiry.reset();
  }
  auto pcols = colsForBuffer();
  switch (action) {
  case Action::Help:
    if (_bufMgr) {
      if (_buf && _buf->readonly() && prevBuf()) {
        setBuffer(*prevBuf());
        prevBuf() = nullptr;
      } else {
        prevBuf() = _buf;
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
    if (requireNotInHistoryMode())
      break;
    prevBuf() = nullptr;
    openSwitchBufferPrompt();
    break;
  case Action::KillBuffer:
    if (requireNotInHistoryMode())
      break;
    if (_buf && _bufMgr && (dynamic_cast<DirBuffer*>(_buf) || !_buf->readonly())) {
      if (!_buf->modified()) {
        auto name = _buf->name();
        if (prevBuf() == _buf)
          prevBuf() = nullptr;
        killBufferAndSwitchPanels(name);
      } else {
        openConfirmKillPrompt(!_buf->hasPath()
                                  ? "Killing temporary buffer. Save to file first? y/n: "
                                  : "Killing modified buffer. Save to file first? y/n: ");
      }
    }
    break;
  case Action::Save:
    if (requireNotInHistoryMode())
      break;
    if (_buf && !_buf->readonly()) {
      if (_buf->hasPath()) {
        if (!std::filesystem::exists(_buf->name())) {
          openConfirmPrompt(
              "File " + escapeNonPrintable(_buf->name()) +
                  " doesn't exist. Really save there? y/n: ",
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
        } else if (auto* tb = dynamic_cast<TextBuffer*>(_buf); tb && tb->changedOnDisk()) {
          openConfirmPrompt(
              "File changed on disk since load. Overwrite anyway? y/n: ",
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
            setStatus("Saved to " + escapeNonPrintable(_buf->name()));
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
      _view->toggleMark();
      setStatus(_view->mark() ? "Mark set" : "Mark cleared");
      _pasteState.reset();
    }
    break;
  case Action::Cut:
    if (_buf && _view->mark()) {
      auto text = _view->extractRegion(_view->mark().value(), _view->point());
      _clipboard.setClipboard(text);
      _pasteRing.push(std::move(text));
      _view->deleteRegion(_view->mark().value(), _view->point());
      setStatus("Region cut");
      _pasteState.reset();
    } else {
      setStatus("No mark set");
    }
    break;
  case Action::Copy:
    if (_buf && _view->mark()) {
      auto text = _view->extractRegion(_view->mark().value(), _view->point());
      _clipboard.setClipboard(text);
      _pasteRing.push(std::move(text));
      _view->clearMark();
      setStatus("Region copied");
      _pasteState.reset();
    } else {
      setStatus("No mark set");
    }
    break;
  case Action::Paste:
    if (_buf && !_buf->readonly()) {
      // Prefer X11 CLIPBOARD when it differs from the top of the
      // internal ring — that way external Ctrl-C from other apps
      // becomes pasteable. The fetched text is also pushed onto the
      // ring so PasteCycle and the paste-ring buffer view see it.
      auto external = _clipboard.getClipboard();
      bool useExternal = external && (_pasteRing.empty() || *external != _pasteRing.at(0));
      if (useExternal)
        _pasteRing.push(*external);
      if (!_pasteRing.empty()) {
        auto start = _view->point();
        auto end = _view->insertText(_pasteRing.at(0));
        _pasteState = PasteState{start, end, 0};
        setStatus("Region pasted");
        _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
      }
    }
    break;
  case Action::PasteCycle:
    if (_buf && !_buf->readonly() && _pasteState && !_pasteRing.empty()) {
      auto nextIdx = (_pasteState->rbIdx + 1) % _pasteRing.size();
      _view->deleteRegion(_pasteState->start, _pasteState->end);
      auto start = _view->point();
      auto end = _view->insertText(_pasteRing.at(nextIdx));
      _pasteState = PasteState{start, end, nextIdx};
      setStatus("Cycling paste " + std::to_string(nextIdx + 1) + "/" +
                std::to_string(_pasteRing.size()));
      _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
    } else {
      setStatus("Can't cycle paste if last command was not a paste");
    }
    break;
  case Action::DeleteChunkBefore:
    if (_buf)
      _view->deleteChunkBeforePoint();
    break;
  case Action::DeleteLine:
    if (_buf) {
      _view->deleteLine();
      _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
      _pasteState.reset();
    }
    break;
  case Action::Find:
    openFindPrompt(false);
    break;
  case Action::FindBackward:
    openFindPrompt(true);
    break;
  case Action::Replace:
    if (!requireNotInHistoryMode())
      openReplacePrompt();
    break;
  case Action::VersionHistory:
    if (_buf && _view) {
      if (_view->inHistoryMode()) {
        _view->exitHistoryModeCommit();
        setStatus("History: committed");
      } else {
        if (_buf->versionHistoryInfo().empty) {
          setStatus("No history");
        } else {
          _view->enterHistoryMode();
          _view->ensureVisible(rowsForBuffer(), pcols, _wrap);
        }
      }
      _pasteState.reset();
    }
    break;
  case Action::Revert:
    if (requireNotInHistoryMode())
      break;
    if (_buf && !_buf->readonly()) {
      if (!_buf->hasPath()) {
        setStatus("Can't revert from file in temporary buffers.");
      } else {
        openConfirmPrompt(
            "Really revert buffer from file " + escapeNonPrintable(_buf->name()) + "? y/n: ",
            [this](std::string const& answer) {
              if (answer == "y") {
                if (_buf) {
                  auto res = _buf->revert();
                  if (res) {
                    // revert() only reloads content; reset view state here
                    // so the cursor and selection land in a sensible place.
                    _view->setPoint(0, 0);
                    _view->clearMark();
                    setStatus("Reverted from " + escapeNonPrintable(_buf->name()));
                  } else {
                    setStatus(res.error());
                  }
                }
              }
            },
            [] {});
      }
    }
    break;
  case Action::BufferList:
    if (requireNotInHistoryMode())
      break;
    if (_bufMgr) {
      if (_buf && _buf->readonly() && prevBuf()) {
        setBuffer(*prevBuf());
        prevBuf() = nullptr;
      } else {
        prevBuf() = _buf;
        setBuffer(_bufMgr->bufferListBuffer());
      }
    }
    break;
  case Action::PasteRing:
    if (_bufMgr) {
      if (_buf && _buf->readonly() && prevBuf()) {
        setBuffer(*prevBuf());
        prevBuf() = nullptr;
      } else {
        prevBuf() = _buf;
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
      if (_view->mark()) {
        auto markLine = _view->mark()->line;
        auto pointLine = _view->point().line;
        range = {std::min(markLine, pointLine) + 1, std::max(markLine, pointLine) + 1};
      }
      executeFormat(range);
    }
    break;
  case Action::PipeReplace:
    if (_buf && !_buf->readonly()) {
      if (!_view->mark())
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
    if (requireNotInHistoryMode())
      break;
    if (_bufMgr)
      setBuffer(_bufMgr->openDir(bufferWorkDir()));
    break;
  case Action::SplitVertical:
    if (_layout && _buf) {
      auto* p = _layout->splitFocused(SplitDir::Vertical, *_buf);
      if (p) {
        updateFocusPointers();
        setStatus("Split vertical (panel " + std::to_string(p->id()) + ")");
      } else {
        setStatus("Not enough space to split");
      }
    }
    break;
  case Action::SplitHorizontal:
    if (_layout && _buf) {
      auto* p = _layout->splitFocused(SplitDir::Horizontal, *_buf);
      if (p) {
        updateFocusPointers();
        setStatus("Split horizontal (panel " + std::to_string(p->id()) + ")");
      } else {
        setStatus("Not enough space to split");
      }
    }
    break;
  case Action::ClosePanel:
    if (_layout) {
      if (_layout->count() <= 1) {
        setStatus("Last panel");
      } else {
        // Save view state if this is the last view for the buffer.
        if (_buf && _layout->viewCountFor(*_buf) == 1)
          _layout->focused().saveViewState();
        _layout->closeFocused();
        updateFocusPointers();
      }
    }
    break;
  case Action::FocusPanel1:
  case Action::FocusPanel2:
  case Action::FocusPanel3:
  case Action::FocusPanel4:
  case Action::FocusPanel5:
  case Action::FocusPanel6:
    if (_layout) {
      int id = static_cast<int>(action) - static_cast<int>(Action::FocusPanel1) + 1;
      if (_layout->focusPanel(id))
        updateFocusPointers();
    }
    break;
  case Action::FocusMRUPanel:
    if (_layout) {
      if (_layout->focusMRU())
        updateFocusPointers();
    }
    break;
  case Action::NextBlock:
    blockNext();
    break;
  case Action::PrevBlock:
    blockPrev();
    break;
  }
}
