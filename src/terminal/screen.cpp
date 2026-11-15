#include "screen.hpp"
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_manager.hpp"
#include "buffer/text_buffer.hpp"
#include "byte_constants.hpp"
#include "error_log.hpp"
#include "fs_path.hpp"
#include "key_sequence.hpp"
#include "mouse.hpp"
#include "regex_search.hpp"
#include "replace.hpp"
#include "search.hpp"
#include "utf8.hpp"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief Read a full UTF-8 character from stdin (given the lead byte) and
 *        insert it into the view as a single code point.
 */
static void insertCharWithUtf8(BufferView& view, char c) {
  view.insert(readUtf8Codepoint(static_cast<unsigned char>(c)));
}

/** @brief Write all bytes to @p fd, retrying on partial writes and EINTR. */
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

// SIGTERMs received and not yet acted on: counted by the handler, taken by
// loop(). A count rather than a flag, since the second request means
// something else than the first (see Screen::handleTerminationRequest())
// and two may arrive before the loop gets to either. A lock-free atomic
// is safe to use from a signal handler.
static std::atomic<int> g_termRequests = 0;
static_assert(std::atomic<int>::is_always_lock_free);

/**
 * @brief Build a PromptDisplay summarising the current completions list.
 *
 * Shared by openSaveAsPrompt and openSwitchBufferPrompt: a dim-styled hint
 * after the input, an optional [a/b/c] for a few alternatives, or an
 * "(N matches)" suffix when there are too many. The caller tracks the
 * user's selected index in @p idxSelected so the hint is suppressed
 * mid-tab-cycle.
 */
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
// Find-in-files stops after this many result lines (one per hit, unless a
// file name holds a newline) or bytes of output. Unbounded, grepping '.'
// over a large tree could use up all memory, and a longer listing is no
// use to read anyway.
constexpr std::size_t kGrepMaxHits = 100'000;
constexpr std::size_t kGrepMaxBytes = std::size_t{64} << 20;
// Initial poll timeout when distinguishing bare-Esc from the start of
// an escape sequence on the input stream.
constexpr int kEscDisambiguationMs = 50;
} // namespace

static void handle_sigwinch(int) { g_resized = 1; }
static void handle_sigterm(int) { ++g_termRequests; }

/**
 * @brief Block until stdin is readable or a signal has been caught.
 *
 * Checking the handlers' flags and then blocking in read() would miss a
 * signal that arrives in between until the next key. So SIGWINCH and
 * SIGTERM are held back while the flags are checked and let in again by
 * pselect() as it starts to wait. They are held for that moment only: a
 * blocked signal would be inherited by the commands the editor runs.
 *
 * @return True if stdin is ready to be read (or broken, which the read
 *         reports); false if a handler has set its flag.
 */
static bool waitForInput() {
  sigset_t held, before;
  sigemptyset(&held);
  sigaddset(&held, SIGWINCH);
  sigaddset(&held, SIGTERM);
  sigprocmask(SIG_BLOCK, &held, &before);
  bool ready = false;
  if (!g_resized && g_termRequests == 0) {
    fd_set in;
    FD_ZERO(&in);
    FD_SET(STDIN_FILENO, &in);
    ready =
        pselect(STDIN_FILENO + 1, &in, nullptr, nullptr, nullptr, &before) >= 0 || errno != EINTR;
  }
  sigprocmask(SIG_SETMASK, &before, nullptr);
  return ready;
}

/**
 * @brief Read a single byte from stdin, retrying on EINTR.
 *
 * Used by the escape-sequence and prompt input paths so a SIGWINCH mid-CSI
 * doesn't drop the next byte and corrupt arrow-key dispatch.
 * @return True on a successful 1-byte read, false on EOF or non-EINTR error.
 */
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

/**
 * @brief Drain a suspected escape-sequence tail after a leading Esc byte
 *        and return a short human-readable label for status display.
 *
 * Handles the case where the user typed Esc then an arrow (arriving as
 * `\x1b\x1b[B`). Returns nullopt when no further bytes arrive within the
 * disambiguation window — the caller treats that as a genuine bare
 * Esc-Esc.
 *
 * The returned label is fed into setStatus() and so must not contain raw
 * control bytes: a pasted `\x1b\x1b[\x1b]X` sequence would otherwise put
 * the terminal into OSC parse mode when written through the modeline.
 * Intermediate bytes that could break terminal state are filtered.
 */
static std::optional<std::string> drainEscapeTail() {
  auto safeForStatus = [](unsigned char b) {
    // Allow 0x20..0x7E (printable ASCII) and 0xA0..0xFF (UTF-8 lead /
    // continuation bytes, harmless to the terminal as text). Reject C0
    // controls (incl. BEL 0x07 and ESC 0x1B), DEL, and C1 controls
    // (incl. ST 0x9C) which can otherwise terminate or rewrite
    // surrounding terminal-control sequences.
    return (b >= 0x20 && b != 0x7F && b < 0x80) || b >= 0xA0;
  };
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, kEscDisambiguationMs) <= 0)
    return std::nullopt;
  char first;
  if (!readByte(first))
    return std::nullopt;
  if (first != '[' && first != 'O') {
    // Lone byte after Esc (Alt-key style) — Esc-<char>. Substitute '?'
    // if the byte itself would corrupt the status line (e.g. a third
    // Esc, which would make setStatus("Esc-\x1b not recognised.")
    // start another CSI/OSC sequence in the modeline).
    return std::string(1, safeForStatus(static_cast<unsigned char>(first)) ? first : '?');
  }
  std::string body;
  body += first;
  char b;
  while (readByte(b)) {
    body += safeForStatus(static_cast<unsigned char>(b)) ? b : '?';
    // CSI / SS3 final byte is in the range 0x40..0x7E. Test the
    // un-substituted byte so a real terminator still ends the loop.
    if (b >= 0x40 && b <= 0x7E)
      break;
    // Sanity bound — terminal escape sequences are short. Bail out
    // rather than block forever on a malformed stream.
    if (body.size() > 32)
      break;
  }
  if (body == "[A")
    return std::string("Up");
  if (body == "[B")
    return std::string("Down");
  if (body == "[C")
    return std::string("Right");
  if (body == "[D")
    return std::string("Left");
  if (body == "[H")
    return std::string("Home");
  if (body == "[F")
    return std::string("End");
  if (body == "[5~")
    return std::string("PgUp");
  if (body == "[6~")
    return std::string("PgDn");
  if (body == "[3~")
    return std::string("Delete");
  if (body == "[2~")
    return std::string("Insert");
  return body;
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

  // SIGTERM asks the editor to exit, as Esc-c does, rather than ending it
  // with the unsaved buffers lost (see handleTerminationRequest()). This
  // takes the signal over from SetupTerminal, which only restores the
  // terminal.
  sa.sa_handler = handle_sigterm;
  sigaction(SIGTERM, &sa, nullptr);

  // Pipe-based formatters and similar tools may close their stdin
  // before reading all our input (e.g. `head -n 1`). Default SIGPIPE
  // disposition would terminate the editor outright, skipping the
  // SetupTerminal destructor and leaving the user's terminal in raw
  // mode. Ignoring lets `write` return EPIPE so we can recover.
  signal(SIGPIPE, SIG_IGN);

  // Set the terminal title. The CWD can contain any byte the filesystem
  // allows (anything except '/' and NUL), so an attacker who controls a
  // directory name could otherwise embed BEL or ESC bytes that close
  // this OSC 0 sequence early and chain additional OSC commands
  // (set-title, OSC 52 clipboard, OSC 4 palette, …). Replace C0 controls
  // (incl. BEL 0x07 and ESC 0x1B), DEL, and C1 controls (incl. ST 0x9C)
  // with '?'. escapeNonPrintable is not usable here because it injects
  // ANSI SGR sequences whose ESC bytes would themselves break OSC.
  auto rawCwd = currentDir().string();
  std::string cwd;
  cwd.reserve(rawCwd.size());
  for (unsigned char b : rawCwd) {
    if (b < 0x20 || b == 0x7F || (b >= 0x80 && b < 0xA0))
      cwd += '?';
    else
      cwd += static_cast<char>(b);
  }
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
    // Leave history mode on the old buffer before its view is dropped,
    // without committing: opening an overlay (help, paste ring) must not
    // discard the versions after the one being looked at.
    if (_view)
      _view->exitHistoryModeCancel();
    // Save view state if this is the last view for the old buffer.
    if (_buf && _layout->viewCountFor(*_buf) == 1)
      panel.saveViewState();
    bool restoreState = (_layout->viewCountFor(buf) == 0);
    panel.setBuffer(buf, restoreState);
  }
  finishBufferSwitch(buf);
}

void Screen::finishBufferSwitch(Buffer& buf) {
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

std::optional<std::size_t> Screen::drawModeline(Panel& panel, bool focused) {
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
      auto t = std::chrono::system_clock::to_time_t(tb->history().timestampAt(idx));
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
  std::optional<std::size_t> promptCursorCol;
  if (focused) {
    if (_prompt) {
      status += " | " + _prompt->label;
      status_vwidth = visualWidth(status);
      // The cell under the prompt cursor is drawn in normal video against
      // the reverse-video bar; a confirm prompt shows it after the label.
      // The selected region is inverted against the bar just as the
      // buffer's is against the text, and underlined so the cursor cell
      // at either end stays distinguishable from it.
      //
      // Escape at the display site: the typed-input path filters
      // control bytes, but completion-accept (Tab / Right-arrow) and
      // history replay copy raw filesystem names into `input`, which a
      // crafted filename could use to inject terminal escapes through
      // the modeline. The raw value is kept for onResult/path lookup.
      std::string_view in = _prompt->showInput ? std::string_view(_prompt->input) : "";
      auto pos = std::min(_prompt->cursorPos(), in.size());
      auto cellEnd = pos < in.size() ? pos + charInfoAt(in.data() + pos, in.size() - pos).len : pos;
      auto widthOf = [&](std::size_t from, std::size_t to) {
        return visualWidth(escapeNonPrintable(in.substr(from, to - from)));
      };
      auto cellWidth = cellEnd > pos ? widthOf(pos, cellEnd) : 1;
      // Keep the cursor on screen: drop input from the front, behind an
      // ellipsis, until the cursor cell fits.
      std::size_t start = 0;
      std::string_view ellipsis;
      if (status_vwidth + widthOf(0, pos) + cellWidth > pc.value &&
          status_vwidth + 1 + cellWidth <= pc.value) {
        std::size_t avail = pc.value - status_vwidth - cellWidth - 1;
        // Walk back from the cursor, character by character, while the
        // text still fits. Measuring only what will be shown keeps a
        // draw linear in the input; re-measuring the whole remainder for
        // every dropped character froze the editor for seconds on a
        // 50 KB paste into a prompt.
        start = pos;
        for (std::size_t shown = 0; start > 0;) {
          auto prev = prevClusterStart(in, start);
          auto w = widthOf(prev, start);
          if (shown + w > avail)
            break;
          shown += w;
          start = prev;
        }
        ellipsis = "\xe2\x80\xa6";
      }
      auto cursorCol = status_vwidth + (ellipsis.empty() ? 0 : 1) + widthOf(start, pos);
      if (cursorCol + cellWidth <= pc.value)
        promptCursorCol = rect.col + cursorCol;
      status += ellipsis;

      auto [regionStart, regionEnd] = _prompt->region().value_or(std::pair{pos, pos});
      // Every region and cursor-cell boundary is a cut, so each piece
      // between two cuts is wholly plain, selected, or the cursor cell.
      std::array cuts{start, regionStart, regionEnd, pos, cellEnd, in.size()};
      std::ranges::sort(cuts);
      for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
        auto from = cuts[i], to = cuts[i + 1];
        if (from < start || from == to)
          continue;
        bool inCell = from >= pos && to <= cellEnd;
        bool selected = !inCell && from >= regionStart && to <= regionEnd;
        if (inCell || selected)
          status += ansi::ReverseVideoOff;
        if (selected)
          status += ansi::Underline;
        status += escapeNonPrintable(in.substr(from, to - from));
        if (selected)
          status += ansi::UnderlineOff;
        if (inCell || selected)
          status += ansi::ReverseVideo;
      }
      if (cellEnd == pos) {
        // Cursor past the end of the input: a blank cursor cell.
        status += ansi::ReverseVideoOff;
        status += ' ';
        status += ansi::ReverseVideo;
      }
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
  // Insert/overwrite only applies to editable buffers; read-only buffers
  // (help, directory browser, …) show no indicator.
  //
  // A live cursor count is prefixed to it as "<count>@": three
  // right-aligned digit columns plus the '@', blank while a single cursor
  // is active. The columns are reserved either way so the indicator never
  // shifts as cursors come and go, or as the count crosses 9 / 99. Counts
  // above 999 show "***@" rather than widening the field.
  std::string modeIndicator;
  if (!pbuf.readonly()) {
    std::string cursorField = "    ";
    if (view.multiCursor()) {
      auto n = view.cursorCount();
      std::string num = n > 999 ? "***" : std::to_string(n);
      cursorField = std::string(3 - num.size(), ' ') + num + "@";
    }
    modeIndicator = " " + cursorField + (pbuf.overwriteMode() ? "O" : "I");
  }
  std::string suffix;
  if (!pbuf.readonly())
    suffix += pbuf.mode() == BufferMode::Cpp   ? " [c]"
              : pbuf.mode() == BufferMode::Md  ? " [md]"
              : pbuf.mode() == BufferMode::Jsn ? " [jsn]"
              : pbuf.mode() == BufferMode::Dif ? " [dif]"
              : pbuf.mode() == BufferMode::Sh  ? " [sh]"
              : pbuf.mode() == BufferMode::Yml ? " [yml]"
              : pbuf.mode() == BufferMode::Tml ? " [tml]"
              : pbuf.mode() == BufferMode::Py  ? " [py]"
                                               : " [txt]";
  suffix += pbuf.modified() ? " +" : "  ";
  suffix += modeIndicator;
  auto suffix_vwidth = visualWidth(suffix);
  auto modeIndicator_vwidth = visualWidth(modeIndicator);
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
  } else if (status_vwidth + modeIndicator_vwidth <= pc.value) {
    // No room for a meaningful buffer name, but room for the mode indicator.
    status.append(pc.value - status_vwidth - modeIndicator_vwidth, ' ');
    status += modeIndicator;
  } else if (status_vwidth < pc.value) {
    // No room for the mode indicator — pad the rest with spaces.
    status.append(pc.value - status_vwidth, ' ');
  } else if (status_vwidth > pc.value) {
    // Status itself overflows (e.g. very long prompt); truncate from
    // the back so we never spill past the panel.
    std::size_t keep = 0;
    std::size_t w = 0;
    while (keep < status.size()) {
      auto info = displayCharInfoAt(status.data() + keep, status.size() - keep);
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
  return promptCursorCol;
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
  bool focusedOverwrite = false;
  bool cursorInPrompt = false;

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

      view.ensureVisible(pr, pc, view.wrap());
      auto [cr, cc] = view.render(
          _renderBuf, pr, pc, {.focused = focused, .rowOffset = rect.row, .colOffset = rect.col});

      if (focused) {
        cur_row = RowIdx{rect.row + cr.value};
        cur_col = ColIdx{rect.col + cc.value};
        focusedOverwrite = panel.buffer().overwriteMode();
      }

      if (auto col = drawModeline(panel, focused)) {
        cur_row = RowIdx{rect.row + rect.rows - 1};
        cur_col = ColIdx{*col};
        cursorInPrompt = true;
      }
      placePanelId(panel, multiPanel);
    });
  }

  ansi::appendCursorTo(_renderBuf, cur_row.value, cur_col.value);
  // Cursor shape signals the edit mode: steady block for insert, steady bar
  // for overwrite.
  _renderBuf += focusedOverwrite ? ansi::CursorBar : ansi::CursorBlock;
  // In a prompt the cursor is drawn by drawModeline() as a normal-video
  // cell; the terminal cursor on top of it would invert it back into the
  // bar's colours, so it is parked there hidden.
  _renderBuf += cursorInPrompt ? ansi::HideCursor : ansi::ShowCursor;

  writeAll(STDOUT_FILENO, _renderBuf.data(), _renderBuf.size());
}

Screen::Exit Screen::loop() {
  std::optional<char> pendingChar;
  while (true) {
    // The next input byte; none if a signal was caught instead.
    std::optional<char> input = std::exchange(pendingChar, std::nullopt);
    if (!input && waitForInput()) {
      char c;
      ssize_t n = read(STDIN_FILENO, &c, 1);
      if (n == 0)
        break; // clean EOF — stdin closed, exit loop
      if (n < 0) {
        // EINTR: a signal, which the next wait finds. Anything else is a
        // transient error (e.g. EIO if the controlling tty is severed,
        // or EAGAIN if stdin was somehow flipped to non-blocking). Log
        // and retry rather than treating it as EOF; if it's persistent
        // the next read will fail again.
        if (errno != EINTR)
          logError(std::string("stdin read: ") + std::strerror(errno));
        continue;
      }
      input = c;
    }

    if (input) {
      // Defence in depth: an exception escaping one action must not end
      // the session (main() would exit and drop every unsaved buffer).
      try {
        if (_prompt)
          handlePromptInput(*input);
        else
          handleNormalInput(*input, pendingChar);
      } catch (std::exception const& e) {
        logError(e.what());
        setStatus(std::string("Error: ") + escapeNonPrintable(e.what()));
      }
    } else {
      if (g_resized) {
        g_resized = 0;
        resize();
      }
      for (int n = g_termRequests.exchange(0); n > 0; --n)
        if (handleTerminationRequest())
          return Exit::Emergency;
    }

    // Process pending exit: walk modified buffers or break.
    if (_exitPending && !_prompt) {
      if (tryExit())
        break;
    }
    // The exit flow a SIGTERM asked for is over once it has been cancelled.
    if (!_exitPending)
      _exitBySignal = false;

    if (input)
      syncPrimarySelection();
    draw();
  }
  return Exit::Normal;
}

bool Screen::handleTerminationRequest() {
  if (_exitPending && _exitBySignal)
    return true;
  _exitBySignal = true;
  if (!_exitPending) {
    // Esc-c does nothing while a prompt is open; a request to terminate
    // must not wait for the prompt, so it is cancelled as Esc would.
    abortPrompt();
    _exitPending = true;
  }
  return false;
}

void Screen::abortPrompt() {
  if (!_prompt)
    return;
  auto abortFn = std::move(_prompt->onAbort);
  _prompt.reset();
  if (abortFn)
    abortFn();
}

void Screen::leaveHistoryModeInAllPanels() {
  if (_layout)
    _layout->forEachPanel([](Panel& panel) { panel.view().exitHistoryModeCancel(); });
}

bool Screen::emergencySave() noexcept {
  if (!_bufMgr)
    return true;
  bool allSaved = true;
  auto report = [](std::string line) {
    line = "cx: " + escapeNonPrintable(line) + "\n";
    writeAll(STDOUT_FILENO, line.data(), line.size());
  };
  try {
    // One suffix for all the copies of this run, in UTC: looking up the
    // local time zone is more than a signal handler should attempt.
    auto const now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto const copySuffix = std::format(".unsaved-{:%Y%m%dT%H%M%SZ}", now);
    // A buffer whose history is being browsed shows an old version and
    // is read-only: back to the newest one, which is what gets written.
    leaveHistoryModeInAllPanels();
    for (auto const& name : _bufMgr->bufferNames()) {
      Buffer* buf = _bufMgr->find(name);
      if (!buf || !buf->modified() || buf->readonly())
        continue;
      if (!buf->hasPath()) {
        report("discarded " + name + " (it has no file)");
        continue;
      }
      // To a copy, never over the file: nobody was asked, and in a signal
      // handler the buffer may be damaged or half updated.
      auto const copy = name + copySuffix;
      if (auto res = buf->writeCopy(copy)) {
        report("unsaved " + name + " written to " + copy);
      } else {
        report("could not save " + name + ": " + res.error());
        logError(res.error());
        allSaved = false;
      }
    }
  } catch (...) {
    // Called from a signal handler too, where nothing may get out and
    // there is nobody left to report it to but the user.
    static constexpr std::string_view kFailed = "cx: saving the modified buffers failed\n";
    writeAll(STDOUT_FILENO, kFailed.data(), kFailed.size());
    allSaved = false;
  }
  return allSaved;
}

void Screen::pullExternalClipboard() {
  // Prefer X11 CLIPBOARD when it differs from the top of the internal
  // ring — that way external Ctrl-C from other apps becomes pasteable.
  // The fetched text is pushed onto the ring so PasteCycle and the
  // paste-ring buffer view see it.
  auto external = _clipboard.getClipboard();
  if (external && (_pasteRing.empty() || *external != _pasteRing.at(0)))
    _pasteRing.push(*external);
}

void Screen::syncPrimarySelection() {
  // An open prompt owns the keyboard, so its selection is the one to
  // publish; the buffer's mark is left alone until the prompt closes.
  if (_prompt) {
    if (auto r = _prompt->region(); r && r->first != r->second && _prompt->showInput)
      _clipboard.setPrimary(_prompt->input.substr(r->first, r->second - r->first));
    return;
  }
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

/**
 * @brief Read the remainder of a CSI sequence whose "ESC [" and first
 *        byte @p first have already been consumed.
 *
 * Collects the parameter/intermediate bytes into @p params and stores the
 * final byte (0x40..0x7E) in @p final, so an unhandled sequence is
 * swallowed whole rather than leaking its tail (e.g. ";5C") as input.
 * @return False on EOF/error, or if the sequence is implausibly long.
 */
static bool readCsi(char first, std::string& params, char& final) {
  constexpr std::size_t kMaxCsiParams = 32;
  for (final = first; !isCsiFinal(final);) {
    if (params.size() == kMaxCsiParams)
      return false;
    params += final;
    if (!readByte(final))
      return false;
  }
  // The Linux console sends F1..F5 as "ESC [ [ A" .. "ESC [ [ E": one
  // more byte belongs to the key.
  if (final == '[' && params.empty()) {
    char letter;
    return readByte(letter);
  }
  return true;
}

/**
 * @brief The final byte of an SS3 sequence whose "ESC O" has already been
 *        consumed, or nullopt if nothing follows.
 *
 * Byte-level conflict: "ESC O" is also what Alt-O sends, with nothing
 * after it. As for a bare Esc, the two are told apart by timing: a byte
 * that follows within the disambiguation window belongs to the sequence.
 * No chord is bound to Esc-O, and one must not be without revisiting this.
 */
static std::optional<char> readSs3Final() {
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  char final;
  if (poll(&pfd, 1, kEscDisambiguationMs) <= 0 || !readByte(final))
    return std::nullopt;
  return final;
}

/** @brief The text of @p s up to its first line break; prompt input is single-line. */
static std::string_view firstLine(std::string const& s) {
  return std::string_view(s).substr(0, s.find_first_of("\r\n"));
}

/**
 * @brief Read the text of a bracketed paste whose start marker has already
 *        been consumed, up to the end marker (or EOF), with its line
 *        breaks as '\n'.
 */
static std::string readBracketedPaste() {
  std::string raw;
  for (char c; readByte(c);) {
    raw += c;
    if (c == kPasteEnd.back() && raw.ends_with(kPasteEnd)) {
      raw.resize(raw.size() - kPasteEnd.size());
      break;
    }
  }
  return normalizePastedText(raw);
}

void Screen::handlePromptInput(char c) {
  auto& p = *_prompt;
  // Only a paste-cycle immediately after a paste may replace it.
  auto lastPaste = std::exchange(p.lastPaste, std::nullopt);
  if (c == key::Enter || c == key::Newline) {
    if (!p.showInput)
      return; // Enter is ignored on confirm prompts
    if (!p.input.empty()) {
      _promptHistory.push(p.label, p.input);
      auto resultFn = std::move(p.onResult);
      auto value = std::move(p.input);
      _prompt.reset();
      resultFn(value);
    } else {
      _prompt.reset();
    }
  } else if (c == key::Escape) {
    // Distinguish bare Esc (cancel) from escape sequences.
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, kEscDisambiguationMs) > 0) {
      char next;
      if (!readByte(next))
        return;
      std::string params;
      char first, final;
      if (next == ansi::CsiIntro) {
        if (!readByte(first) || !readCsi(first, params, final))
          return;
        if (isPasteStart(params, final)) {
          // Pasted text: its first line, as for a paste from the ring. A
          // confirm prompt takes no answer from a paste.
          auto text = readBracketedPaste();
          if (p.showInput && !text.empty()) {
            p.insert(firstLine(text));
            p.historyIdx.reset();
            runAutocomplete();
          }
        } else if (auto key = decodeCsiKey(params, final)) {
          handlePromptKey(*key);
        }
      } else if (next == ansi::Ss3Intro) {
        if (auto ss3 = readSs3Final())
          if (auto key = decodeSs3Key(*ss3))
            handlePromptKey(*key);
      } else if (auto action = handleShortcut(shortcutFromEscChar(next))) {
        handlePromptAction(*action, lastPaste);
      }
    } else {
      // Bare Esc: cancel prompt.
      abortPrompt();
    }
  } else if (c == key::Tab && p.onTab) {
    p.display = p.onTab(p.input);
    p.inputReplaced();
    p.historyIdx.reset();
  } else if (c == key::Backspace) {
    // With a mark set, Backspace deletes the region, as in the buffer.
    // Swallow the byte when there is nothing to delete — otherwise it
    // would fall into the printable branch below (Backspace is 0x7F,
    // which is >= PrintableMin) and silently end up in the prompt input.
    if (!(p.mark ? p.deleteRegion() : p.deleteBefore()))
      return;
    p.historyIdx.reset();
    runAutocomplete();
  } else if (static_cast<unsigned char>(c) >= key::PrintableMin) {
    std::string ch(1, c);
    auto u = static_cast<unsigned char>(c);
    if (u >= 0x80)
      readUtf8Tail(u, ch);
    if (ch.empty())
      return; // malformed UTF-8, dropped by readUtf8Tail
    p.insert(ch);
    p.historyIdx.reset();
    runAutocomplete();
  } else if (p.onSpecialKey && p.onSpecialKey(c, p.input)) {
    p.inputReplaced();
    runAutocomplete();
  } else if (auto action = handleShortcut(shortcutFromCtrlChar(c))) {
    handlePromptAction(*action, lastPaste);
  }
}

void Screen::handlePromptAction(Action action, std::optional<Prompt::Paste> lastPaste) {
  auto& p = *_prompt;
  if (!p.showInput)
    return;
  bool edited = false;
  switch (action) {
  case Action::ToggleMark:
    p.toggleMark();
    break;
  case Action::Cut:
  case Action::Copy:
    if (auto r = p.region(); r && r->first != r->second) {
      auto text = p.input.substr(r->first, r->second - r->first);
      _clipboard.setClipboard(text);
      _pasteRing.push(std::move(text));
      if (action == Action::Cut)
        edited = p.deleteRegion();
    }
    p.mark.reset();
    break;
  case Action::Paste:
    pullExternalClipboard();
    if (!_pasteRing.empty()) {
      auto start = p.cursorPos();
      p.insert(firstLine(_pasteRing.at(0)));
      p.lastPaste = Prompt::Paste{start, p.cursorPos(), 0};
      edited = true;
    }
    break;
  case Action::PasteCycle:
    if (lastPaste && !_pasteRing.empty()) {
      auto idx = (lastPaste->ringIdx + 1) % _pasteRing.size();
      p.erase(lastPaste->start, lastPaste->end);
      p.insert(firstLine(_pasteRing.at(idx)));
      p.lastPaste = Prompt::Paste{lastPaste->start, p.cursorPos(), idx};
      edited = true;
    }
    break;
  case Action::DeleteChunkBefore:
    edited = p.deleteChunkBefore();
    break;
  default:
    break; // Other bindings have no meaning inside a prompt.
  }
  if (edited) {
    p.historyIdx.reset();
    runAutocomplete();
  }
}

void Screen::handlePromptKey(KeyPress key) {
  auto& p = *_prompt;
  bool plain = key.plain();
  bool ctrl = key.ctrlOnly();

  // Up/Down browse history (the input is a single line, so they have no
  // cursor meaning). History replaces the input; the cursor goes to its end.
  if (plain && key.key == Key::Up) {
    auto* hist = _promptHistory.get(p.label);
    if (!hist)
      return;
    if (!p.historyIdx) {
      p.savedInput = p.input;
      p.historyIdx = 0;
      p.input = (*hist)[0];
    } else if (*p.historyIdx + 1 < hist->size()) {
      ++*p.historyIdx;
      p.input = (*hist)[*p.historyIdx];
    }
    p.inputReplaced();
    runAutocomplete();
    return;
  }
  if (plain && key.key == Key::Down) {
    if (!p.historyIdx)
      return;
    if (*p.historyIdx == 0) {
      p.input = p.savedInput;
      p.historyIdx.reset();
    } else {
      --*p.historyIdx;
      if (auto* hist = _promptHistory.get(p.label))
        p.input = (*hist)[*p.historyIdx];
    }
    p.inputReplaced();
    runAutocomplete();
    return;
  }

  // Line editing only applies to prompts that show their input: confirm
  // prompts react to the last key typed, so their hidden input must stay
  // append-only.
  if (!p.showInput)
    return;
  bool edited = false;
  switch (key.key) {
  case Key::Right:
    if (ctrl)
      p.moveWordRight();
    else if (plain && !p.cursorAtEnd())
      p.moveRight();
    else if (plain && p.onRightArrow) {
      // Right at the end of the input accepts the completion instead.
      p.display = p.onRightArrow(p.input);
      p.inputReplaced();
    }
    break;
  case Key::Left:
    if (ctrl)
      p.moveWordLeft();
    else if (plain)
      p.moveLeft();
    break;
  case Key::Home:
    if (plain || ctrl)
      p.moveHome();
    break;
  case Key::End:
    if (plain || ctrl)
      p.moveEnd();
    break;
  case Key::Delete:
    if (plain)
      edited = p.mark ? p.deleteRegion() : p.deleteAt();
    else if (ctrl)
      edited = p.deleteChunkAt();
    break;
  default:
    break; // Other keys have no meaning inside a prompt.
  }
  if (edited) {
    p.historyIdx.reset();
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
            // An exists() error (e.g. untraversable parent) falls through to
            // the save, which reports the real failure.
            std::error_code existsEc;
            if (std::filesystem::exists(resolved, existsEc)) {
              // Defer rename until the user confirms the overwrite.
              auto savedName = _buf->name();
              // Escape the path: tab-completion may have produced a
              // filename containing control bytes, which would otherwise
              // be rendered verbatim in the prompt label.
              openConfirmPrompt(
                  "Overwrite " + escapeNonPrintable(resolved) + "? y/n: ",
                  [this, savedName, resolved](std::string const& answer) {
                    if (answer == "y") {
                      if (_buf && _bufMgr && renameBufferAndRekeyBookmarks(savedName, resolved))
                        saveAndMaybeKill();
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
            if (renameBufferAndRekeyBookmarks(_buf->name(), resolved))
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
              // A directory is reopened, which re-reads its listing.
              setBuffer(dynamic_cast<DirBuffer*>(target) ? _bufMgr->openDir(target->name())
                                                         : *target);
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
        if (input.empty())
          return buildDimHint(_lastSearch);
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

void Screen::openGrepPrompt() {
  _prompt = Prompt{
      .label = "Grep: ",
      .onResult = [this](std::string const& value) { runGrep(value); },
  };
}

std::filesystem::path Screen::searchStartDir() const {
  if (auto* db = dynamic_cast<DirBuffer*>(_buf))
    return db->directory();
  if (_buf && _buf->hasPath()) {
    auto dir = std::filesystem::path(_buf->name()).parent_path();
    if (!dir.empty())
      return dir;
  }
  return currentDir();
}

void Screen::runGrep(std::string const& pattern) {
  if (pattern.empty() || !_bufMgr)
    return;

  auto start = searchStartDir();

  // A work tree gets `git grep`, which honours .gitignore and so keeps
  // build output and vendored trees out of the results. Anything else
  // falls back to a plain recursive grep.
  auto topLevel = runPipeCommand({"git", "rev-parse", "--show-toplevel"}, "", start.string());
  bool inRepo = topLevel && !topLevel->killed && topLevel->exitCode == 0;
  std::filesystem::path root = start;
  if (inRepo) {
    auto out = topLevel->stdoutData;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
      out.pop_back();
    if (out.empty())
      inRepo = false;
    else
      root = out;
  }

  // -e guards a pattern that begins with '-' from being read as a flag.
  std::vector<std::string> args;
  if (inRepo)
    args = {"git", "grep", "-I", "-n", "-z", "-e", pattern};
  else
    args = {"grep", "-r", "-I", "-n", "-Z", "--exclude-dir=.git", "-e", pattern, "."};

  setStatus("Searching...");
  draw();
  auto res =
      runPipeCommand(args, "", root.string(), {.lines = kGrepMaxHits, .bytes = kGrepMaxBytes});
  if (!res)
    return;
  if (res->killed) {
    setStatus("Grep cancelled");
    return;
  }
  // Both backends exit 1 for "no matches" and >1 for a real failure.
  // 127 is runPipeCommand's own marker for a failed exec. A search
  // stopped at the limit has no exit code of its own.
  if (!res->truncated && res->exitCode == 127) {
    setStatus("No grep available");
    return;
  }
  if (!res->truncated && res->exitCode > 1) {
    auto err = res->stderrData;
    auto nl = err.find('\n');
    if (nl != std::string::npos)
      err.resize(nl);
    setStatus(err.empty() ? "Grep failed" : "Grep: " + escapeNonPrintable(err));
    return;
  }

  auto hits = parseGrepOutput(res->stdoutData, root);
  auto count = hits.size();
  auto& gb = _bufMgr->grepBuffer(pattern, root, std::move(hits), res->truncated);
  // Record the predecessor so `q` dismisses the listing, as it does for
  // the other read-only overlays. Re-running grep from inside the
  // results must not make the listing its own predecessor.
  if (_buf && _buf != &gb)
    prevBuf() = _buf;
  setBuffer(gb);
  if (count == 0)
    setStatus("No matches for " + escapeNonPrintable(pattern));
  else
    setStatus(std::to_string(count) + (count == 1 ? " match" : " matches") +
              (res->truncated ? " (search stopped at the limit)" : ""));
}

void Screen::openGrepHit(GrepHit const& hit) {
  if (!_bufMgr || !_layout)
    return;

  Buffer& target = _bufMgr->open(hit.path());

  // Show the hit beside the results rather than on top of them, so the
  // listing survives walking several matches. A split that does not fit
  // falls back to reusing the current panel.
  if (_layout->count() == 1) {
    if (auto* p = _layout->splitFocused(SplitDir::Vertical, target)) {
      _layout->focusPanel(p->id());
      updateFocusPointers();
    }
  } else {
    if (_layout->focusMRU())
      updateFocusPointers();
  }
  setBuffer(target);

  std::size_t line = hit.line > 0 ? hit.line - 1 : 0;
  if (line >= target.lineCount())
    line = target.lineCount() > 0 ? target.lineCount() - 1 : 0;
  _view->setPoint(line, 0);
  // Centre rather than ensureVisible(), which scrolls the minimum amount
  // and so leaves the hit pinned to the bottom row with no context below
  // it. recenter() clamps by itself near the start of the file.
  auto rows = rowsForBuffer();
  _view->recenter(rows, rows.value / 2, colsForBuffer(), _view->wrap());
  _pasteState.reset();
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
                // Escaped: the pattern may be pasted text holding control
                // bytes, which the label would write to the terminal raw.
                .label = "Replace '" + escapeNonPrintable(pattern) + "' with: ",
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
  auto m = findNextMatchInBuffer(*_buf, _view->point(), pattern);
  if (!m) {
    _view->clearHighlight();
    _highlightExpiry.reset();
    setStatus("Replaced " + std::to_string(replaced) + " occurrence(s)");
    return;
  }
  _view->setPoint(m->first.line, m->first.offset);
  _view->setHighlight(m->first, m->second);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
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
              _view->replaceRegion(matchStart, matchEnd, replacement);
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
              _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
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

void Screen::regexFindNext() {
  if (!_buf || _lastRegex.empty())
    return;
  std::string err;
  auto re = compileRegex(_lastRegex, err);
  if (!re) {
    setStatus("Bad regex: " + escapeNonPrintable(err));
    return;
  }
  std::size_t lineCount = _buf->lineCount();
  std::size_t startLine = _view->point().line;
  std::size_t startOff = _view->point().offset;
  bool wrapped = false;
  for (std::size_t count = 0; count < lineCount; ++count) {
    std::size_t li = (startLine + count) % lineCount;
    if (count > 0 && li <= startLine)
      wrapped = true;
    std::size_t from = (count == 0) ? startOff : 0;
    auto const& line = _buf->lineContent(li);
    auto m = findFirstRegex(line, from, *re);
    // An empty match at the cursor is the one just found (onFindMatch puts
    // the cursor at the match end): skip it, or find-next never advances.
    if (m && count == 0 && m->first == m->second && m->first == startOff)
      m = startOff < line.size()
              ? findFirstRegex(
                    line, startOff + utf8SeqLen(static_cast<unsigned char>(line[startOff])), *re)
              : std::nullopt;
    if (m) {
      onFindMatch(li, m->first, m->second, wrapped);
      return;
    }
  }
  // Wrap-around tail: check the start of the original line.
  if (auto m = findFirstRegex(_buf->lineContent(startLine), 0, *re)) {
    if (m->first < startOff) {
      onFindMatch(startLine, m->first, m->second, true);
      return;
    }
  }
  _view->clearHighlight();
  _highlightExpiry.reset();
  setStatus("Not found");
}

void Screen::openRegexFindPrompt() {
  if (!_lastRegex.empty() && _buf)
    regexFindNext();
  _prompt = Prompt{
      .label = "Regex find: ",
      .autocomplete = [this](std::string const& input) -> Prompt::AutocompleteResult {
        if (input.empty())
          return buildDimHint(_lastRegex);
        return PromptDisplay{"", 0};
      },
      .onResult =
          [this](std::string const& value) {
            if (!_buf)
              return;
            _lastRegex = value;
            regexFindNext();
          },
  };
  runAutocomplete();
}

void Screen::openRegexReplacePrompt() {
  if (!_buf || _buf->readonly()) {
    setStatus("Buffer is read-only");
    return;
  }
  _prompt = Prompt{
      .label = "Regex pattern: ",
      .onResult =
          [this](std::string const& pattern) {
            if (pattern.empty() || !_buf)
              return;
            // Validate the pattern up front so the user sees the parse
            // error immediately rather than after typing the
            // replacement template.
            std::string err;
            if (!compileRegex(pattern, err)) {
              setStatus("Bad regex: " + escapeNonPrintable(err));
              return;
            }
            _lastRegex = pattern;
            _prompt = Prompt{
                // Escaped for the same reason as in openReplacePrompt().
                .label = "Replace /" + escapeNonPrintable(pattern) + "/ with: ",
                .onResult =
                    [this, pattern](std::string const& replacement) {
                      regexReplacePump(pattern, replacement, 0);
                    },
            };
          },
  };
}

void Screen::regexReplacePump(std::string pattern, std::string replacement, std::size_t replaced) {
  if (!_buf || _buf->readonly())
    return;
  std::string err;
  auto re = compileRegex(pattern, err);
  if (!re) {
    setStatus("Bad regex: " + escapeNonPrintable(err));
    return;
  }
  auto m = findNextRegexInBuffer(*_buf, _view->point(), *re);
  if (!m) {
    _view->clearHighlight();
    _highlightExpiry.reset();
    setStatus("Replaced " + std::to_string(replaced) + " occurrence(s)");
    return;
  }
  _view->setPoint(m->first.line, m->first.offset);
  _view->setHighlight(m->first, m->second);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
  _highlightExpiry = std::chrono::steady_clock::now() + kHighlightTtl;
  openRegexReplaceConfirmPrompt(std::move(pattern), std::move(replacement), replaced, m->first,
                                m->second);
}

void Screen::openRegexReplaceConfirmPrompt(std::string pattern, std::string replacement,
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
              std::string err;
              auto re = compileRegex(pattern, err);
              if (!re) {
                setStatus("Bad regex: " + escapeNonPrintable(err));
                return;
              }
              auto& line = _buf->lineContent(matchStart.line);
              std::string rendered =
                  substituteCaptures(line, matchStart.offset, matchEnd.offset, *re, replacement);
              _view->replaceRegion(matchStart, matchEnd, rendered);
              ++newReplaced;
            } else {
              _view->setPoint(matchEnd.line, matchEnd.offset);
            }
            // Resume past an empty match, or the next search finds it again.
            if (matchStart == matchEnd) {
              auto next = stepPastEmptyMatch(*_buf, _view->point());
              if (!next) {
                _view->clearHighlight();
                _highlightExpiry.reset();
                setStatus("Replaced " + std::to_string(newReplaced) + " occurrence(s)");
                return;
              }
              _view->setPoint(next->line, next->offset);
            }
            if (c == '!') {
              std::string err;
              auto re = compileRegex(pattern, err);
              if (!re) {
                setStatus("Bad regex: " + escapeNonPrintable(err));
                return;
              }
              auto extra = replaceAllRegexForward(*_buf, _view->point(), *re, replacement);
              newReplaced += extra;
              _view->clampToBuffer();
              _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
              _view->clearHighlight();
              _highlightExpiry.reset();
              setStatus("Replaced " + std::to_string(newReplaced) + " occurrence(s)");
              return;
            }
            regexReplacePump(pattern, replacement, newReplaced);
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
      .label = "Mode [txt/c/md/jsn/dif/sh/yml/tml/py]: ",
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
            } else if (value == "sh" || value == "shell" || value == "bash") {
              if (_buf)
                _buf->setMode(BufferMode::Sh);
              setStatus("Mode: sh");
            } else if (value == "yml" || value == "yaml") {
              if (_buf)
                _buf->setMode(BufferMode::Yml);
              setStatus("Mode: yml");
            } else if (value == "tml" || value == "toml") {
              if (_buf)
                _buf->setMode(BufferMode::Tml);
              setStatus("Mode: tml");
            } else if (value == "py" || value == "python") {
              if (_buf)
                _buf->setMode(BufferMode::Py);
              setStatus("Mode: py");
            } else {
              setStatus("Unknown mode: " + escapeNonPrintable(value) +
                        " (use 'txt', 'c', 'md', 'jsn', 'dif', 'sh', 'yml', 'tml', or 'py')");
            }
          },
  };
}

void Screen::openSetBookmarkPrompt() {
  _prompt = Prompt{
      .label = "Set bookmark (0-9 a-z): ",
      .showInput = false,
      .autocomplete = [](std::string const& input) -> Prompt::AutocompleteResult {
        if (!input.empty() && Bookmarks::validSlot(input.back()))
          return std::string{input.back()};
        return PromptDisplay{"", 0};
      },
      .onResult =
          [this](std::string const& value) {
            if (value.empty() || !_buf || !_view)
              return;
            // Only buffers the manager can resolve by name can be jumped
            // back to; the singleton overlays (*help*, *buffers*,
            // *paste ring*) live outside its maps, so a bookmark in one
            // could never be honoured and would be silently dropped by
            // the next jump. Refuse it here, while the user can still act.
            if (!_bufMgr || !_bufMgr->find(_buf->name())) {
              setStatus("Cannot bookmark this buffer");
              return;
            }
            char slot = value[0];
            bool existed = _bookmarks.contains(slot);
            _bookmarks.set(slot, _buf->name(), _view->point());
            setStatus(std::string("Bookmark '") + slot + (existed ? "' replaced" : "' set"));
          },
  };
}

void Screen::openJumpBookmarkPrompt() {
  auto entries = _bookmarks.list();
  if (entries.empty()) {
    setStatus("No bookmarks set");
    return;
  }
  std::string hint;
  for (auto& [slot, mark] : entries) {
    if (!hint.empty())
      hint += ' ';
    hint += slot;
    hint += ':';
    hint += _bufMgr ? _bufMgr->displayName(mark.bufferName) : mark.bufferName;
  }
  // The hint is fixed for the prompt's lifetime, so build it once rather
  // than re-escaping and re-measuring it on every ignored keystroke.
  auto dimHint = buildDimHint(hint);
  _prompt = Prompt{
      .label = "Jump to bookmark: ",
      .showInput = false,
      .autocomplete = [dimHint](std::string const& input) -> Prompt::AutocompleteResult {
        if (!input.empty() && Bookmarks::validSlot(input.back()))
          return std::string{input.back()};
        return dimHint;
      },
      .onResult =
          [this](std::string const& value) {
            if (!value.empty())
              jumpToBookmark(value[0]);
          },
      .display = dimHint,
  };
}

void Screen::jumpToBookmark(char slot) {
  if (!_bufMgr)
    return;
  auto mark = _bookmarks.get(slot);
  if (!mark) {
    setStatus(std::string("No bookmark '") + slot + "'");
    return;
  }
  Buffer* target = _bufMgr->find(mark->bufferName);
  if (!target) {
    _bookmarks.erase(slot);
    setStatus(std::string("Bookmark '") + slot + "': buffer gone");
    return;
  }
  if (target != _buf) {
    prevBuf() = nullptr;
    setBuffer(*target);
  }
  // setPoint() clamps both line and offset to the current buffer
  // contents, which is exactly the staleness rule bookmarks want:
  // edits are not tracked, positions are clamped lazily on jump.
  _view->setPoint(mark->point.line, mark->point.offset);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
}

bool Screen::renameBufferAndRekeyBookmarks(std::string oldName, std::string const& newName) {
  // oldName must be a copy, not a reference into _buf->name(): rename()
  // mutates the buffer's name in place via setPath(), so a reference
  // would silently alias the new name by the time renameBuffer() below
  // reads it back.
  if (!_bufMgr)
    return false;
  Buffer* renamed = _bufMgr->find(oldName);
  if (!renamed)
    return false; // nothing registered under oldName; rename() would no-op too
  if (!_bufMgr->rename(oldName, newName)) {
    setStatus("Already open in another buffer: " + escapeNonPrintable(newName));
    _killPending = false;
    _exitPending = false;
    return false;
  }
  // Read the post-rename name off the renamed buffer itself rather than
  // off _buf: rename() derives the new key internally via setPath(), and
  // the renamed buffer is not necessarily the focused one.
  _bookmarks.renameBuffer(oldName, renamed->name());
  return true;
}

void Screen::openExecuteCommandPrompt() {
  // Snapshot the command list once per prompt; the binding table is
  // constexpr so it never changes during a session, but rebuilding the
  // sorted vector at construction time keeps the autocomplete callback
  // simple.
  struct State {
    std::vector<std::pair<std::string_view, Action>> commands;
    // Filtered candidates after substring match against the user's
    // input. Indices into `commands`. Lowercase comparison.
    std::vector<std::size_t> matches;
    std::optional<std::size_t> selected;
  };
  auto state = std::make_shared<State>();
  state->commands = allCommands();

  auto lower = [](std::string s) {
    for (auto& c : s)
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c + 32);
    return s;
  };

  auto rebuildMatches = [state, lower](std::string const& input) {
    state->matches.clear();
    state->selected.reset();
    if (input.empty()) {
      // Empty input: list everything in declaration order, but cap the
      // initial display at a sensible amount via the hint logic below.
      for (std::size_t i = 0; i < state->commands.size(); ++i)
        state->matches.push_back(i);
      return;
    }
    std::string needle = lower(input);
    for (std::size_t i = 0; i < state->commands.size(); ++i) {
      auto desc = lower(std::string(state->commands[i].first));
      if (desc.find(needle) != std::string::npos)
        state->matches.push_back(i);
    }
  };

  auto displayFor = [state](std::string const& input) -> PromptDisplay {
    if (state->matches.empty()) {
      if (input.empty())
        return {"", 0};
      auto txt = std::string(" (no match)");
      return {std::string(ansi::Dim) + txt + std::string(ansi::DimOff),
              static_cast<std::size_t>(txt.size())};
    }
    if (state->selected) {
      // While cycling with Tab, mute the inline hint — the input field
      // already shows the chosen command.
      return {"", 0};
    }
    auto top = state->commands[state->matches[0]].first;
    std::string hint;
    if (top.starts_with(input))
      hint = std::string(top.substr(input.size()));
    else
      hint = std::string(top);
    if (state->matches.size() > 1)
      hint += " (" + std::to_string(state->matches.size()) + " matches)";
    return {std::string(ansi::Dim) + hint + std::string(ansi::DimOff), visualWidth(hint)};
  };

  rebuildMatches("");

  _prompt = Prompt{
      .label = "Command: ",
      .autocomplete = [state, rebuildMatches,
                       displayFor](std::string const& input) -> Prompt::AutocompleteResult {
        rebuildMatches(input);
        return displayFor(input);
      },
      .onResult =
          [this, state](std::string const& value) {
            // Resolve to an action: prefer the cycled selection, else
            // a single remaining match, else look up the literal value.
            std::optional<Action> resolved;
            if (state->selected && *state->selected < state->matches.size())
              resolved = state->commands[state->matches[*state->selected]].second;
            else if (state->matches.size() == 1)
              resolved = state->commands[state->matches[0]].second;
            else {
              for (auto& [desc, a] : state->commands)
                if (desc == value) {
                  resolved = a;
                  break;
                }
            }
            if (!resolved) {
              setStatus("No command matches: " + escapeNonPrintable(value));
              return;
            }
            // Run the action, then (if it has one) surface the
            // shortcut so the user can learn the binding.
            auto a = *resolved;
            auto desc = descriptionForAction(a);
            dispatchAction(a);
            auto shortcut = formatShortcut(shortcutForAction(a));
            // Actions with no chord (Insert key, Ctrl-Shift-Up/Down) have
            // no binding to teach, so leave the hint off entirely.
            if (shortcut.empty()) {
              if (_statusMsg.empty())
                setStatus(std::string(desc));
              return;
            }
            // If the dispatched action set its own status, don't
            // overwrite it — append the binding hint instead.
            std::string hint = std::string(desc) + " (" + shortcut + ")";
            if (!_statusMsg.empty() && _statusMsg != hint)
              setStatus(_statusMsg + "  [" + shortcut + "]");
            else
              setStatus(hint);
          },
      .onTab = [state, displayFor](std::string& input) -> PromptDisplay {
        if (state->matches.empty())
          return {"", 0};
        if (!state->selected)
          state->selected = 0;
        else
          state->selected = (*state->selected + 1) % state->matches.size();
        input = state->commands[state->matches[*state->selected]].first;
        return displayFor(input);
      },
  };
  runAutocomplete();
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
  // The focused view still references the destroyed buffer: replace it
  // directly. setBuffer() would save view state into (and exit history
  // mode on) the dead buffer.
  _layout->focused().setBuffer(replacement, true);
  finishBufferSwitch(replacement);
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
    setStatus(escapeNonPrintable(res.error()));
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
                setStatus(escapeNonPrintable(res.error()));
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

/** @brief Append all currently-available bytes from @p fd to @p out. */
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
  if (_buf && _buf->hasPath()) {
    auto dir = std::filesystem::path(_buf->name()).parent_path();
    if (!dir.empty())
      return dir.string();
  }
  return currentDir().string();
}

std::optional<Screen::PipeResult> Screen::runPipeCommand(std::vector<std::string> const& args,
                                                         std::string_view input,
                                                         std::string const& workDir,
                                                         PipeLimit limit) {
  if (args.empty())
    return std::nullopt;

  int pipeIn[2], pipeOut[2], pipeErr[2];
  if (pipe(pipeIn) != 0) {
    setStatus("pipe: " + escapeNonPrintable(std::strerror(errno)));
    return std::nullopt;
  }
  if (pipe(pipeOut) != 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    setStatus("pipe: " + escapeNonPrintable(std::strerror(errno)));
    return std::nullopt;
  }
  if (pipe(pipeErr) != 0) {
    close(pipeIn[0]);
    close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeOut[1]);
    setStatus("pipe: " + escapeNonPrintable(std::strerror(errno)));
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
    setStatus("fork: " + escapeNonPrintable(std::strerror(errno)));
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
  // Feeding the child must never block. A blocking write() of a large
  // input returns only once all of it is taken, and a filter that streams
  // (cat, tr, sed) stops reading as soon as its own output pipe is full —
  // which this loop can only drain between writes. With O_NONBLOCK a write
  // takes what fits and poll() says when there is room for more.
  fcntl(pipeIn[1], F_SETFL, fcntl(pipeIn[1], F_GETFL) | O_NONBLOCK);

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

  // True once stdout has reached the limit; cuts it after the last whole
  // line within it. Lines are counted incrementally from `scanned`.
  std::size_t scanned = 0, outLines = 0;
  auto reachedLimit = [&] {
    for (auto it = stdoutData.begin() + scanned;
         (it = std::find(it, stdoutData.end(), '\n')) != stdoutData.end(); ++it) {
      if (++outLines == limit.lines) {
        stdoutData.erase(it + 1, stdoutData.end());
        return true;
      }
    }
    scanned = stdoutData.size();
    if (stdoutData.size() < limit.bytes)
      return false;
    auto nl = stdoutData.rfind('\n', limit.bytes - 1);
    stdoutData.resize(nl == std::string::npos ? 0 : nl + 1);
    return true;
  };
  // Kill the command at the limit. No drain: the output is not wanted, and
  // a grandchild could hold the pipes open.
  auto limitReturn = [&]() -> PipeResult {
    kill(pid, SIGKILL);
    if (stdinOpen)
      close(pipeIn[1]);
    close(pipeOut[0]);
    close(pipeErr[0]);
    int wstatus;
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {
    }
    _statusMsg.clear();
    _statusExpiry.reset();
    return {std::move(stdoutData), std::move(stderrData), -1, false, true};
  };

  // Poll set is rebuilt each iteration because stdinOpen, the pipes' EOF
  // state and killPromptShowing can change. A pipe at EOF is left out: it
  // stays readable (POLLHUP) forever, so polling it would spin without
  // ever reaching the timeout that offers to kill a command which closed
  // one of its outputs (`cmd 2>/dev/null`) and kept running.
  std::vector<pollfd> fds;
  fds.reserve(4);
  bool outDone = false, errDone = false;
  int status = 0;
  bool reaped = false;
  int childPollMs = 1;

  for (;;) {
    // With every pipe closed only the child's exit is left to wait for,
    // and no descriptor reports that. Check it between short polls
    // (backing off to 50 ms) rather than in a blocking waitpid(), so a
    // command that closed both outputs and kept running can be killed too.
    bool onlyChild = outDone && errDone && !stdinOpen;
    if (onlyChild) {
      pid_t wp = waitpid(pid, &status, WNOHANG);
      if (wp == pid) {
        reaped = true;
        break;
      }
      if (wp < 0 && errno != EINTR)
        break;
    }

    fds.clear();
    if (stdinOpen)
      fds.push_back({pipeIn[1], POLLOUT, 0});
    if (!outDone)
      fds.push_back({pipeOut[0], POLLIN, 0});
    if (!errDone)
      fds.push_back({pipeErr[0], POLLIN, 0});
    if (killPromptShowing)
      fds.push_back({STDIN_FILENO, POLLIN, 0});

    int timeoutMs = -1;
    if (!killPromptShowing) {
      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      timeoutMs = std::max(0, static_cast<int>(remaining.count()));
    }
    if (onlyChild) {
      timeoutMs = timeoutMs < 0 ? childPollMs : std::min(timeoutMs, childPollMs);
      childPollMs = std::min(childPollMs * 2, 50);
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
      // Just a tick of the child-exit check?
      if (killPromptShowing || std::chrono::steady_clock::now() < deadline)
        continue;
      // Timeout: show kill prompt.
      setStatus("Command still running. Kill? y/n");
      draw();
      killPromptShowing = true;
      continue;
    }

    for (auto& pfd : fds) {
      if (pfd.fd == pipeIn[1] && (pfd.revents & (POLLOUT | POLLERR))) {
        if (written < input.size()) {
          auto n = write(pipeIn[1], input.data() + written, input.size() - written);
          if (n > 0)
            written += static_cast<std::size_t>(n);
          else if (n < 0 && errno != EINTR && errno != EAGAIN) {
            close(pipeIn[1]);
            stdinOpen = false;
          }
        } else {
          close(pipeIn[1]);
          stdinOpen = false;
        }
      }
      if (pfd.fd == pipeOut[0]) {
        readPipe(pipeOut[0], pfd.revents, stdoutData, outDone);
        if (reachedLimit())
          return limitReturn();
      }
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
              // A negative fd is ignored by poll(): skip pipes at EOF so
              // the child really gets its 100 ms per round.
              pollfd drain[] = {{outDone ? -1 : pipeOut[0], POLLIN, 0},
                                {errDone ? -1 : pipeErr[0], POLLIN, 0}};
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
              readPipe(pipeOut[0], drain[0].revents, stdoutData, outDone);
              readPipe(pipeErr[0], drain[1].revents, stderrData, errDone);
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
  }

  if (stdinOpen)
    close(pipeIn[1]);

  drainFd(pipeOut[0], stdoutData);
  drainFd(pipeErr[0], stderrData);
  close(pipeOut[0]);
  close(pipeErr[0]);
  bool truncated = reachedLimit();

  if (!reaped) {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);

  _statusMsg.clear();
  _statusExpiry.reset();
  return PipeResult{std::move(stdoutData), std::move(stderrData), exitCode, false, truncated};
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
    // Escape: the child's stderr is attacker-controlled (malicious
    // clang-format on PATH, or a normal one echoing parts of file
    // content) and would otherwise inject terminal escape sequences
    // through the modeline.
    setStatus(escapeNonPrintable(nl != std::string::npos ? err.substr(0, nl) : err));
    return;
  }
  if (res->stdoutData == allText) {
    setStatus("Already formatted");
    return;
  }

  auto savedLine = _view->point().line;
  auto savedOffset = _view->point().offset;
  _view->replaceRegion({0, 0}, endPt, res->stdoutData);
  auto line = std::min(savedLine, _buf->lineCount() - 1);
  auto offset = std::min(savedOffset, _buf->lineContent(line).size());
  _view->setPoint(line, offset);
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
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

  // The output replaces the region; with no output, what the command
  // wrote to stderr does, so a failed command is not a silent deletion.
  std::string const stderrText = res->stderrData.empty() ? "" : "STDERR:\n" + res->stderrData;
  _view->replaceRegion(from, to, res->stdoutData.empty() ? stderrText : res->stdoutData);
  _view->clearMark();

  if (!res->stdoutData.empty() && !res->stderrData.empty()) {
    std::string trimmed;
    trimmed.reserve(res->stderrData.size());
    for (char c : res->stderrData)
      if (c != '\n' && c != '\r')
        trimmed += c;
    // Escape: the child's stderr is attacker-controlled and could
    // otherwise inject terminal escape sequences through the modeline.
    setStatus("Command returned error output: " + escapeNonPrintable(trimmed));
  }

  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
}

void Screen::handleEscapeInput(std::optional<char>& pendingChar) {
  struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
  if (poll(&pfd, 1, 50) > 0) {
    // Escape sequence: read the next bytes to identify the key.
    char seq[2];
    if (!readByte(seq[0]))
      return;
    if (seq[0] == ansi::CsiIntro) {
      if (!readByte(seq[1]))
        return;
      // SGR mouse: ESC[<b;col;row(M|m). Detected by '<' immediately
      // after the CSI intro (no other CSI command starts with '<').
      if (seq[1] == '<') {
        auto body = collectSgrMouseBody([](char& out) { return readByte(out); });
        if (auto e = parseSgrMouse(body))
          handleMouseEvent(*e);
        return;
      }
      // Read the sequence to its end whatever it is: a key cx has no use
      // for (a function key, Ctrl-PgUp, ...) is then dropped whole instead
      // of having its tail typed into the buffer.
      std::string params;
      char final;
      if (!readCsi(seq[1], params, final))
        return;
      if (isPasteStart(params, final)) {
        // Pasted text goes in as one edit. A read-only buffer drops it:
        // typed, its characters would have run as that buffer's commands.
        auto text = readBracketedPaste();
        if (_buf && !_buf->readonly() && !text.empty()) {
          _view->insertText(text);
          _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
          _pasteState.reset();
        }
      } else if (auto key = decodeCsiKey(params, final)) {
        handleKeyPress(*key);
      }
    } else if (auto ss3 = seq[0] == ansi::Ss3Intro ? readSs3Final() : std::nullopt) {
      // SS3: arrows and Home/End in application-cursor mode; F1..F4.
      if (auto key = decodeSs3Key(*ss3))
        handleKeyPress(*key);
    } else if (seq[0] == key::Escape) {
      // Esc-Esc arrived as a contiguous byte pair. The second Esc may
      // itself be the start of a real escape sequence the user pressed
      // after Esc (e.g. Esc then Down arrow). Try to drain that tail
      // and report it as unrecognised; fall back to the standard
      // Esc-Esc handler when nothing follows.
      if (auto name = drainEscapeTail())
        setStatus("Esc-" + *name + " not recognised.");
      else
        handleBareEscEsc();
    } else if (!handleEscChord(seq[0])) {
      // Not a key sequence or known chord — insert the byte verbatim
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
        if (g_termRequests > 0)
          break; // a request to terminate ends the wait: back to loop()
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
        // Same as the fast path above: the trailing Esc may actually
        // be the prefix of an arrow/PgUp/etc. sequence the user pressed
        // after a deliberately-paused Esc. Drain any tail before
        // assuming a genuine Esc-Esc.
        if (auto name = drainEscapeTail())
          setStatus("Esc-" + *name + " not recognised.");
        else
          handleBareEscEsc();
      } else {
        // Escape: `next` is an arbitrary input byte, which formatShortcut
        // would otherwise splat as a raw control char into the modeline.
        setStatus(escapeNonPrintable(formatShortcut(shortcutFromEscChar(next))) +
                  " not recognised.");
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

void Screen::handleKeyPress(KeyPress key) {
  if (!_buf)
    return;
  auto pcols = colsForBuffer();
  if (key.plain()) {
    switch (key.key) {
    case Key::Up:
      if (_view->wrap())
        _view->moveUp(pcols);
      else
        _view->movePhysicalUp();
      if (_view->point() < _view->viewFrom())
        _view->scrollUp(_view->wrap());
      break;
    case Key::Down: {
      auto prev = _view->point();
      if (_view->wrap())
        _view->moveDown(pcols);
      else
        _view->movePhysicalDown();
      if (_view->point() == prev)
        _view->scrollViewNearEnd(rowsForBuffer());
      else
        _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      break;
    }
    case Key::Right:
      _view->moveRight();
      break;
    case Key::Left:
      _view->moveLeft();
      break;
    case Key::Home:
      _view->moveHome();
      break;
    case Key::End:
      _view->moveEnd();
      break;
    case Key::PageUp:
      _view->pageUp(rowsForBuffer(), pcols, _view->wrap());
      break;
    case Key::PageDown:
      _view->pageDown(rowsForBuffer(), pcols, _view->wrap());
      break;
    case Key::Insert:
      dispatchAction(Action::ToggleOverwriteMode);
      break;
    case Key::Delete:
      if (_view->mark())
        _view->deleteRegion(_view->mark().value(), _view->point());
      else
        _view->deleteAtPoint();
      _pasteState.reset();
      break;
    }
  } else if (key.ctrlOnly()) {
    switch (key.key) {
    case Key::Right:
      _view->moveWordRight();
      break;
    case Key::Left:
      _view->moveWordLeft();
      break;
    case Key::Up:
      if (_view->wrap() && _view->currentLineWraps(pcols))
        _view->movePhysicalUp();
      else
        _view->moveParagraphUp();
      break;
    case Key::Down:
      if (_view->wrap() && _view->currentLineWraps(pcols))
        _view->movePhysicalDown();
      else
        _view->moveParagraphDown();
      break;
    case Key::Home:
      _view->setPoint(0, 0);
      break;
    case Key::End:
      _view->setPoint(_buf->lineCount() - 1, _buf->lineContent(_buf->lineCount() - 1).size());
      break;
    case Key::Delete:
      _view->deleteChunkAtPoint();
      break;
    default:
      break;
    }
    _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
  } else if (key.ctrlShiftOnly()) {
    // Shift+Ctrl+Up/Down grow the cursor set by a column.
    if (key.key == Key::Up)
      dispatchAction(Action::AddCursorUp);
    else if (key.key == Key::Down)
      dispatchAction(Action::AddCursorDown);
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
    if (_view && _view->inHistoryMode()) {
      // History mode: Enter commits the version shown, like Ctrl-u.
      dispatchAction(Action::VersionHistory);
    } else if (blb && prevBuf() && _bufMgr) {
      auto name = blb->selectedName(_view->point());
      Buffer* target = _bufMgr->find(name);
      if (target) {
        prevBuf() = nullptr;
        // A directory is reopened, which re-reads its listing.
        setBuffer(dynamic_cast<DirBuffer*>(target) ? _bufMgr->openDir(name) : *target);
      }
    } else if (auto* gb = dynamic_cast<GrepBuffer*>(_buf); gb && _bufMgr) {
      // Grep buffer: Enter opens the file behind the selected result.
      if (auto hit = gb->selectedHit(_view->point()))
        openGrepHit(*hit);
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
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
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
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
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
        _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      return;
    }
    // '?' on a bracket → jump to matching bracket. Works in all modes:
    // Cpp/Jsn use mode-aware matchers (skipping brackets in strings,
    // matching template <>); Md/Dif/Txt fall back to ()[]{} matching
    // (the underlying Cpp matcher only treats <> as brackets when
    // tagged Angle, so plain-text comparison operators stay inert).
    if (c == '?' && _buf && !_buf->readonly() && _view->gotoMatchingBracket()) {
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
      } else if (c == 'r') {
        auto pt = _view->point();
        if (pt.line >= db->entries().size()) {
          setStatus("No entry selected");
        } else {
          auto const& entry = db->entries()[pt.line];
          if (entry.name == "." || entry.name == "..") {
            setStatus("Cannot rename . or ..");
          } else {
            std::string saved = entry.name;
            _prompt = Prompt{
                .label = "Rename to: ",
                .input = saved,
                .onResult =
                    [this, pt](std::string const& newName) {
                      auto* db2 = dynamic_cast<DirBuffer*>(_buf);
                      if (!db2)
                        return;
                      auto result = db2->renameEntry(pt, newName);
                      _view->setPoint(result.hint.point.line, result.hint.point.offset);
                      if (result.ok)
                        setStatus("Renamed");
                      else
                        setStatus("Rename failed: " + escapeNonPrintable(result.error));
                    },
            };
          }
        }
      } else if (c == 'm') {
        _prompt = Prompt{
            .label = "New directory: ",
            .onResult =
                [this](std::string const& newName) {
                  auto* db2 = dynamic_cast<DirBuffer*>(_buf);
                  if (!db2)
                    return;
                  auto result = db2->makeSubdir(newName);
                  _view->setPoint(result.hint.point.line, result.hint.point.offset);
                  if (result.ok)
                    setStatus("Created " + escapeNonPrintable(newName));
                  else
                    setStatus("mkdir failed: " + escapeNonPrintable(result.error));
                },
        };
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

void Screen::handleMouseEvent(MouseEvent const& event) {
  if (!_layout)
    return;
  // Convert 1-based screen coords to 0-based.
  if (event.col == 0 || event.row == 0)
    return;
  std::size_t col = event.col - 1;
  std::size_t row = event.row - 1;

  bool isWheel = event.isWheelUp() || event.isWheelDown();

  // Drag continuation. Left-motion and the terminating left-release
  // belong to a drag already in progress; act on the focused panel
  // (where the drag began) without re-targeting by coordinates, so
  // dragging onto the modeline / gutter / a sibling panel keeps
  // extending the selection instead of jumping.
  if (!isWheel && event.isLeftButton() && event.isRelease()) {
    _dragAnchor.reset();
    return;
  }
  if (!isWheel && event.isLeftButton() && event.isMotion()) {
    if (_dragAnchor && _view)
      extendDragSelection(row, col);
    return;
  }

  // Past here only fresh presses (and wheel) are actionable: a stray
  // release or a motion without the left button held carries no intent.
  if (!isWheel && (!event.isPress() || event.isMotion()))
    return;
  if (!isWheel && !event.isLeftButton() && !event.isMiddleButton())
    return;

  // Find the panel containing (row, col).
  Panel* hit = nullptr;
  _layout->forEachPanel([&](Panel& p) {
    auto const& r = p.rect();
    if (row >= r.row && row < r.row + r.rows && col >= r.col && col < r.col + r.cols)
      hit = &p;
  });
  if (!hit)
    return;

  // Focus the hit panel if it's not already focused. Switching focus
  // updates the MRU stack and refreshes _buf/_view.
  if (hit->id() != _layout->focused().id()) {
    _layout->focusPanel(hit->id());
    updateFocusPointers();
  }

  auto const& rect = hit->rect();
  auto pcols = ColIdx{rect.cols};

  if (isWheel) {
    constexpr std::size_t kWheelLines = 3;
    auto pcontentRows = hit->contentRows();
    for (std::size_t i = 0; i < kWheelLines; ++i) {
      if (event.isWheelUp())
        _view->scrollUp(_view->wrap());
      else
        _view->scrollDown();
    }
    // Drag the cursor along so it never falls outside the new viewport.
    // Otherwise the next keypress's ensureVisible() would snap the view
    // back to the (off-screen) cursor and undo the user's scroll. Use
    // pointForScreenRowCol to derive the first and last visible buffer
    // points; any column 0 keeps the new cursor at the line head.
    auto first = _view->pointForScreenRowCol(0, 0, pcols, _view->wrap());
    auto last = _view->pointForScreenRowCol(pcontentRows.value - 1, 0, pcols, _view->wrap());
    auto pt = _view->point();
    if (pt < first)
      _view->setPoint(first.line, first.offset);
    else if (last < pt)
      _view->setPoint(last.line, last.offset);
    return;
  }

  if (event.isMiddleButton()) {
    // X11 middle-click paste, served in-app: our SGR mouse grab stops
    // the terminal performing its native PRIMARY paste, so we do it
    // ourselves. Per design the selection lands at the current cursor,
    // not the click position. Mirror Action::Paste's ring handling so
    // PasteCycle and the paste-ring buffer see it.
    if (_buf && !_buf->readonly()) {
      if (auto sel = _clipboard.getPrimary()) {
        if (_pasteRing.empty() || *sel != _pasteRing.at(0))
          _pasteRing.push(*sel);
        auto start = _view->point();
        auto end = _view->insertText(_pasteRing.at(0));
        _pasteState = PasteState{start, end, 0};
        setStatus("Pasted selection");
        _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      } else {
        setStatus("Selection empty");
      }
    }
    return;
  }

  // Left press: convert to a buffer point and move the cursor there,
  // then arm a drag anchored at that point. A plain click deliberately
  // leaves any existing mark intact (a keyboard-set mark survives a
  // click elsewhere); only a subsequent drag installs a new mark. Any
  // half-finished previous drag is dropped first.
  _dragAnchor.reset();
  // Ignore presses on the modeline (last row) — it carries no buffer
  // content.
  if (row + 1 >= rect.row + rect.rows)
    return;
  // Skip the gutter column — it carries wrap/scroll indicators, not
  // buffer text.
  if (col + 1 >= rect.col + rect.cols)
    return;

  std::size_t screenRow = row - rect.row;
  std::size_t screenCol = col - rect.col;
  auto pt = _view->pointForScreenRowCol(screenRow, screenCol, pcols, _view->wrap());
  _view->setPoint(pt.line, pt.offset);
  _dragAnchor = _view->point();
  _pasteState.reset();
}

void Screen::extendDragSelection(std::size_t row, std::size_t col) {
  Panel& p = _layout->focused();
  auto const& rect = p.rect();
  auto pcols = ColIdx{rect.cols};
  auto contentRows = p.contentRows();
  if (contentRows.value == 0)
    return;

  // Vertical: clamp the pointer into the content band. At or above the
  // top content row scroll up; at or below the bottom one scroll down —
  // so a drag can run past the viewport. The terminal can't report a
  // row above 0, so scrolling at the edge row is the only way to reach
  // off-screen content; the cost is that selecting exactly the edge
  // line scrolls one extra line, which is the conventional trade-off.
  std::size_t screenRow;
  if (row <= rect.row) {
    screenRow = 0;
    _view->scrollUp(_view->wrap());
  } else if (row >= rect.row + contentRows.value - 1) {
    screenRow = contentRows.value - 1;
    _view->scrollDown();
  } else {
    screenRow = row - rect.row;
  }

  // Horizontal: clamp into the content columns. The final column is the
  // gutter (wrap/scroll indicators), so content is [0, cols-2].
  std::size_t maxCol = rect.cols >= 2 ? rect.cols - 2 : 0;
  std::size_t screenCol = col <= rect.col ? 0 : col - rect.col;
  if (screenCol > maxCol)
    screenCol = maxCol;

  auto pt = _view->pointForScreenRowCol(screenRow, screenCol, pcols, _view->wrap());
  // The drag overrides any prior (e.g. keyboard-set) mark: the live
  // region is exactly anchor→point. setMark is idempotent so repeating
  // it every motion event is free.
  _view->setMark(*_dragAnchor);
  _view->setPoint(pt.line, pt.offset);
  _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
  _pasteState.reset();
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

void Screen::handleBareEscEsc() {
  // Collapsing extra cursors comes first: while multiple cursors are
  // active that is overwhelmingly what Esc-Esc is meant to undo, and a
  // second Esc-Esc still reaches the overlay/mark duties below.
  if (_view && _view->multiCursor()) {
    _view->collapseCursors();
    setStatus("Single cursor");
    return;
  }
  // History mode: back to the newest version, dropping nothing.
  if (_view && _view->inHistoryMode()) {
    _view->exitHistoryModeCancel();
    _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
    _pasteState.reset();
    setStatus("History: cancelled");
    return;
  }
  // Overlay dismissal takes precedence over mark clearing: the mark
  // belongs to the underlying buffer and is preserved across an
  // overlay, so a second Esc-Esc after dismissal still clears it.
  if (_buf && _buf->readonly() && prevBuf()) {
    setBuffer(*prevBuf());
    prevBuf() = nullptr;
  } else if (_view && _view->mark()) {
    _view->clearMark();
    setStatus("Mark cleared");
  }
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
      if (g_termRequests > 0)
        break; // a request to terminate ends the wait: back to loop()
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
      // Escape: `next` is an arbitrary input byte; formatShortcut would
      // otherwise splat it as a raw control char into the modeline.
      setStatus(escapeNonPrintable(formatShortcut(s)) + " not recognised.");
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

  // A buffer whose history is being browsed is read-only, and would be
  // skipped below with its unsaved changes. Leave history mode in every
  // panel first, so each buffer is back at its newest version.
  leaveHistoryModeInAllPanels();

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
  _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
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

/**
 * @brief True if @p line is a block-navigation target: a diff hunk header
 *        (in diff mode) or a merge-conflict marker (when conflicts exist).
 */
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
      _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
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
      _view->ensureVisible(rowsForBuffer(), colsForBuffer(), _view->wrap());
      if (li >= startLine)
        setStatus("Block (wrapped)");
      return;
    }
  }
  setStatus("No blocks found");
}

void Screen::dispatchAction(Action action) {
  if (action != Action::Find && action != Action::FindBackward && action != Action::RegexFind &&
      action != Action::RegexReplace && _buf) {
    _view->clearHighlight();
    _highlightExpiry.reset();
  }
  // Reset the recenter cycle on any action other than Recenter itself,
  // so the next Ctrl-l starts at the middle slot again.
  if (action != Action::Recenter)
    _lastRecenter.reset();
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
  case Action::SelectAllOccurrences:
    if (_buf) {
      if (auto n = _view->selectAllOccurrences())
        setStatus("Created " + std::to_string(n) + (n == 1 ? " cursor" : " cursors"));
      else
        setStatus("No word at cursor");
    }
    break;
  case Action::AddCursorDown:
    if (_buf) {
      // The running total lives in the modeline indicator, so the status
      // line only reports what this keystroke did.
      if (_view->addCursorDown())
        setStatus("Cursor added");
      else
        setStatus("No line below");
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
    }
    break;
  case Action::AddCursorUp:
    if (_buf) {
      if (_view->addCursorUp())
        setStatus("Cursor added");
      else
        setStatus("No line above");
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
    }
    break;
  case Action::SetBookmark:
    if (!requireNotInHistoryMode())
      openSetBookmarkPrompt();
    break;
  case Action::JumpBookmark:
    if (!requireNotInHistoryMode())
      openJumpBookmarkPrompt();
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
        // Only ask when the file is known to be missing; an exists() error
        // (e.g. the directory lost its x bit) goes straight to save(),
        // which reports it.
        std::error_code existsEc;
        if (!std::filesystem::exists(_buf->name(), existsEc) && !existsEc) {
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
            setStatus(escapeNonPrintable(res.error()));
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
      pullExternalClipboard();
      if (!_pasteRing.empty()) {
        auto start = _view->point();
        auto end = _view->insertText(_pasteRing.at(0));
        _pasteState = PasteState{start, end, 0};
        setStatus("Region pasted");
        _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      }
    }
    break;
  case Action::PasteCycle:
    if (_buf && !_buf->readonly() && _pasteState && !_pasteRing.empty()) {
      auto nextIdx = (_pasteState->rbIdx + 1) % _pasteRing.size();
      auto start = std::min(_pasteState->start, _pasteState->end);
      auto end = _view->replaceRegion(_pasteState->start, _pasteState->end, _pasteRing.at(nextIdx));
      _pasteState = PasteState{start, end, nextIdx};
      setStatus("Cycling paste " + std::to_string(nextIdx + 1) + "/" +
                std::to_string(_pasteRing.size()));
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
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
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
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
        } else if (!_view->enterHistoryMode()) {
          setStatus("History is being browsed in another panel");
        } else {
          _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
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
                    setStatus(escapeNonPrintable(res.error()));
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
    if (_view) {
      _view->toggleShowWhitespace();
      setStatus(_view->showWhitespace() ? "Whitespace on" : "Whitespace off");
    }
    break;
  case Action::ToggleWrap:
    if (_view) {
      _view->toggleWrap();
      setStatus(_view->wrap() ? "Wrap on" : "Wrap off");
    }
    break;
  case Action::ToggleCurrentLine:
    if (_view) {
      _view->toggleHighlightCurrentLine();
      setStatus(_view->highlightCurrentLine() ? "Current-line highlight on"
                                              : "Current-line highlight off");
    }
    break;
  case Action::ToggleLineNumbers:
    if (_view) {
      _view->toggleShowLineNumbers();
      setStatus(_view->showLineNumbers() ? "Line numbers on" : "Line numbers off");
    }
    break;
  case Action::ToggleMouseTracking: {
    // Toggle the terminal's mouse-reporting modes (press/release +
    // motion + SGR). When off, the terminal regains its native
    // right-click context menu and selection behaviour. In-app
    // left-click/drag, middle-click paste and wheel scrolling all
    // depend on tracking being on, so the user can flip back when
    // they want the terminal's own handling instead.
    _mouseEnabled = !_mouseEnabled;
    std::string_view seq = _mouseEnabled ? std::string_view("\x1b[?1000h\x1b[?1002h\x1b[?1006h")
                                         : std::string_view("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    writeAll(STDOUT_FILENO, seq.data(), seq.size());
    setStatus(_mouseEnabled ? "Mouse tracking on"
                            : "Mouse tracking off (Shift+click also bypasses)");
    break;
  }
  case Action::ToggleOverwriteMode:
    if (_buf && !_buf->readonly()) {
      _buf->toggleOverwriteMode();
      setStatus(_buf->overwriteMode() ? "Overwrite mode" : "Insert mode");
    }
    break;
  case Action::ExecuteCommand:
    openExecuteCommandPrompt();
    break;
  case Action::Reflow:
    if (!requireNotInHistoryMode() && _buf && !_buf->readonly()) {
      _view->reflowParagraph(80, _buf->mode() == BufferMode::Md ? BufferView::ReflowTables::Markdown
                                                                : BufferView::ReflowTables::Ignore);
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      _pasteState.reset();
    }
    break;
  case Action::Uppercase:
  case Action::Lowercase:
  case Action::Capitalize:
    if (!requireNotInHistoryMode() && _buf && !_buf->readonly()) {
      _view->convertCase(action == Action::Uppercase   ? BufferView::CaseOp::Upper
                         : action == Action::Lowercase ? BufferView::CaseOp::Lower
                                                       : BufferView::CaseOp::Capitalize);
      _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
      _pasteState.reset();
    }
    break;
  case Action::RegexFind:
    openRegexFindPrompt();
    break;
  case Action::RegexReplace:
    if (!requireNotInHistoryMode())
      openRegexReplacePrompt();
    break;
  case Action::ToggleComment:
    if (requireNotInHistoryMode())
      break;
    if (_buf && !_buf->readonly()) {
      // Mode-defined line-comment prefix. Markdown / JSON / Diff lack a
      // line-comment form so we surface a status hint instead of
      // silently picking the wrong syntax.
      if (_buf->mode() == BufferMode::Cpp) {
        _view->toggleComment("// ");
        _view->ensureVisible(rowsForBuffer(), pcols, _view->wrap());
        _pasteState.reset();
      } else {
        setStatus("No line-comment syntax for this mode");
      }
    }
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
  case Action::Grep:
    if (requireNotInHistoryMode())
      break;
    openGrepPrompt();
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
        // The view is about to be dropped: leave history mode, or the
        // buffer stays read-only with nothing left to release it.
        if (_view)
          _view->exitHistoryModeCancel();
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
  case Action::Recenter:
    if (_buf && _view) {
      // Cycle: middle (1st press) → top → bottom → middle → ...
      RecenterSlot slot = !_lastRecenter                           ? RecenterSlot::Middle
                          : *_lastRecenter == RecenterSlot::Middle ? RecenterSlot::Top
                          : *_lastRecenter == RecenterSlot::Top    ? RecenterSlot::Bottom
                                                                   : RecenterSlot::Middle;
      auto rows = rowsForBuffer();
      std::size_t pos = slot == RecenterSlot::Middle ? rows.value / 2
                        : slot == RecenterSlot::Top  ? 0
                                                     : (rows.value > 0 ? rows.value - 1 : 0);
      _view->recenter(rows, pos, pcols, _view->wrap());
      _lastRecenter = slot;
    }
    break;
  }
}
