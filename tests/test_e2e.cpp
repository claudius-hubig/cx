#define BOOST_TEST_MODULE E2E
#include <boost/test/unit_test.hpp>

#include "test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

// Path to the cx binary, relative to the build directory.
static std::string cxBinary() {
  // Try common locations.
  if (std::filesystem::exists("./cx"))
    return "./cx";
  if (std::filesystem::exists("../build/cx"))
    return "../build/cx";
  // Fallback: assume ctest runs from the build dir.
  return "./cx";
}

// RAII wrapper around a child process running in a PTY.
struct PtySession {
  int masterFd = -1;
  pid_t childPid = -1;

  PtySession(std::size_t rows, std::size_t cols, std::string const& binary,
             std::vector<std::string> const& args = {}) {
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);

    childPid = forkpty(&masterFd, nullptr, nullptr, &ws);
    if (childPid == 0) {
      // Child process: exec the editor.
      std::vector<char*> argv;
      // Need writable copies for execv.
      std::string binCopy = binary;
      argv.push_back(binCopy.data());
      std::vector<std::string> argCopies(args.begin(), args.end());
      for (auto& a : argCopies)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      execv(binary.c_str(), argv.data());
      _exit(127); // exec failed
    }
    BOOST_REQUIRE(childPid > 0);
    BOOST_REQUIRE(masterFd >= 0);
  }

  ~PtySession() {
    if (childPid > 0) {
      // SIGKILL: a SIGTERM only asks the editor to exit, and it asks back
      // when a buffer is modified.
      kill(childPid, SIGKILL);
      int status = 0;
      waitpid(childPid, &status, 0);
    }
    if (masterFd >= 0)
      close(masterFd);
  }

  PtySession(PtySession const&) = delete;
  PtySession& operator=(PtySession const&) = delete;

  void send(std::string_view keys) { ::write(masterFd, keys.data(), keys.size()); }

  // Read from the PTY, accumulating output until `timeout` elapses with
  // no new data (or total timeout is hit).
  std::string readOutput(std::chrono::milliseconds timeout = std::chrono::milliseconds{500}) {
    std::string result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
    char buf[4096];

    while (std::chrono::steady_clock::now() < deadline) {
      struct pollfd pfd{};
      pfd.fd = masterFd;
      pfd.events = POLLIN;

      int ms = static_cast<int>(timeout.count());
      int ret = poll(&pfd, 1, ms);
      if (ret <= 0)
        break;
      if (pfd.revents & POLLIN) {
        ssize_t n = ::read(masterFd, buf, sizeof(buf));
        if (n <= 0)
          break;
        result.append(buf, n);
      } else {
        break;
      }
    }
    return result;
  }

  // Wait until output contains the expected string, or timeout.
  std::string readUntil(std::string_view expected,
                        std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    std::string result;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char buf[4096];

    while (std::chrono::steady_clock::now() < deadline) {
      struct pollfd pfd{};
      pfd.fd = masterFd;
      pfd.events = POLLIN;

      auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      int ms = std::max(1, static_cast<int>(remaining.count()));
      int ret = poll(&pfd, 1, ms);
      if (ret <= 0)
        continue;
      if (pfd.revents & POLLIN) {
        ssize_t n = ::read(masterFd, buf, sizeof(buf));
        if (n <= 0)
          break;
        result.append(buf, n);
        if (result.find(expected) != std::string::npos)
          return result;
      }
    }
    return result;
  }

  void resize(std::size_t rows, std::size_t cols) {
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ioctl(masterFd, TIOCSWINSZ, &ws);
    kill(childPid, SIGWINCH);
  }

  // Wait for child to exit with timeout. Returns exit status or -1.
  int waitExit(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      pid_t ret = waitpid(childPid, &status, WNOHANG);
      if (ret == childPid) {
        childPid = -1; // Don't kill in destructor.
        if (WIFEXITED(status))
          return WEXITSTATUS(status);
        return -1;
      }
      usleep(10000); // 10ms
    }
    return -1; // timed out
  }
};

// --- E2E tests ---

BOOST_AUTO_TEST_CASE(e2e_starts_and_exits) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  // Wait for initial render.
  auto output = session.readOutput(std::chrono::milliseconds{300});
  BOOST_CHECK(!output.empty());

  // Send Esc-c to exit.
  session.send("\x1b"
               "c");
  int exitCode = session.waitExit();
  BOOST_CHECK_EQUAL(exitCode, 0);
}

BOOST_AUTO_TEST_CASE(e2e_list_buffer_hides_cursor) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  auto dir =
      std::filesystem::temp_directory_path() / ("cx_e2e_hide_cursor_" + std::to_string(::getpid()));
  std::filesystem::create_directory(dir);
  std::ofstream(dir / "file.txt") << "x\n";

  // A directory listing shows its cursor as a reverse-video row, so the
  // terminal cursor is hidden once the frame is drawn.
  PtySession session(24, 80, bin, {dir.string()});
  auto out = session.readUntil("file.txt", std::chrono::milliseconds{1000});
  out += session.readOutput(std::chrono::milliseconds{200});
  auto hide = out.rfind("\x1b[?25l");
  auto show = out.rfind("\x1b[?25h");
  BOOST_CHECK(hide != std::string::npos);
  BOOST_CHECK(show == std::string::npos || hide > show);

  // Quitting from there must give the shell its cursor back.
  session.send("\x1b"
               "c");
  auto end = session.readOutput(std::chrono::milliseconds{300});
  auto teardown = end.find("\x1b[?1049l");
  BOOST_REQUIRE(teardown != std::string::npos);
  BOOST_CHECK_MESSAGE(end.find("\x1b[?25h", teardown) != std::string::npos, "cursor left hidden");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  std::filesystem::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(e2e_type_and_display) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Type "hello".
  session.send("hello");
  auto output = session.readUntil("hello");
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("hello") != std::string::npos);

  // Exit without saving: Esc-c, then 'n' to discard.
  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_insert_key_toggles_overwrite_mode) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Press the Insert key (ESC[2~) to switch to overwrite mode. The status
  // line reports the switch and the cursor turns into a steady bar.
  session.send("\x1b[2~");
  auto over = session.readUntil("Overwrite mode", std::chrono::milliseconds{1000});
  BOOST_CHECK_MESSAGE(over.find("Overwrite mode") != std::string::npos,
                      "Expected 'Overwrite mode' status: " + stripAnsi(over));
  BOOST_CHECK_MESSAGE(over.find("\x1b[6 q") != std::string::npos,
                      "Expected steady-bar cursor (ESC[6 q) in overwrite mode");

  // Press Insert again to return to insert mode: status flips back and the
  // cursor returns to a steady block.
  session.send("\x1b[2~");
  auto ins = session.readUntil("Insert mode", std::chrono::milliseconds{1000});
  BOOST_CHECK_MESSAGE(ins.find("Insert mode") != std::string::npos,
                      "Expected 'Insert mode' status: " + stripAnsi(ins));
  BOOST_CHECK_MESSAGE(ins.find("\x1b[2 q") != std::string::npos,
                      "Expected steady-block cursor (ESC[2 q) in insert mode");

  // Toggling the mode does not modify the buffer, so Esc-c exits directly.
  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

BOOST_AUTO_TEST_CASE(e2e_esc_uppercase_u_uppercases_word) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Type a word, move back onto it, then press Esc-U (uppercase letter).
  session.send("hello");
  session.readUntil("hello");
  session.send("\x1b[D\x1b[D"); // two left arrows, cursor now inside the word
  session.send("\x1b"
               "U");
  auto out = session.readUntil("HELLO");
  auto stripped = stripAnsi(out);
  BOOST_CHECK_MESSAGE(stripped.find("HELLO") != std::string::npos,
                      "Expected uppercased 'HELLO' in: " + stripped);

  // Exit without saving.
  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_esc_then_arrow_does_not_insert_csi_bytes) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Esc + Down arrow. Previously the trailing "[B" leaked through to
  // the buffer as printable text; expect the status line to report
  // "Esc-Down not recognised" and nothing to be inserted.
  session.send("\x1b\x1b[B");
  auto output = session.readUntil("not recognised");
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Esc-Down not recognised") != std::string::npos);
  BOOST_CHECK(stripped.find("[B") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_mouse_drag_selects_region_and_cut_removes_it) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Put a unique token on the only line. A leading space means a
  // press at terminal column 1 anchors at or before the token, so a
  // drag to the far right selects the whole token regardless of the
  // line-number gutter width.
  session.send(" ZZZUNIQUEZZZ");
  session.readUntil("ZZZUNIQUEZZZ");

  // Left-press at (row 1, col 1), drag (left held = button 32) to the
  // far right of the line, then release. mark→point now spans the line.
  session.send("\x1b[<0;1;1M");
  session.readOutput(std::chrono::milliseconds{80});
  session.send("\x1b[<32;79;1M");
  session.readOutput(std::chrono::milliseconds{80});
  session.send("\x1b[<0;79;1m");
  session.readOutput(std::chrono::milliseconds{80});

  // Ctrl-W cuts the dragged region; the token must be gone.
  session.send("\x17");
  auto stripped = stripAnsi(session.readOutput(std::chrono::milliseconds{300}));
  BOOST_CHECK(stripped.find("ZZZUNIQUEZZZ") == std::string::npos);

  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_stdin_pipe_seeds_scratch_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Spawn cx with stdin connected to a pipe (not a TTY) and stdout/stderr
  // connected to a PTY. cx must drain the pipe, then re-open /dev/tty so
  // the input loop still works. Verify the piped content shows up.
  int pipeFd[2];
  BOOST_REQUIRE(pipe(pipeFd) == 0);

  int masterFd = -1, slaveFd = -1;
  struct winsize ws{};
  ws.ws_row = 24;
  ws.ws_col = 80;
  BOOST_REQUIRE(openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws) == 0);

  pid_t pid = fork();
  BOOST_REQUIRE(pid >= 0);
  if (pid == 0) {
    // Child: stdin from the pipe; stdout/stderr to the PTY slave; the PTY
    // slave is the controlling tty after setsid().
    setsid();
    ioctl(slaveFd, TIOCSCTTY, 0);
    dup2(pipeFd[0], STDIN_FILENO);
    dup2(slaveFd, STDOUT_FILENO);
    dup2(slaveFd, STDERR_FILENO);
    close(pipeFd[0]);
    close(pipeFd[1]);
    close(slaveFd);
    close(masterFd);
    char* argv[] = {const_cast<char*>(bin.c_str()), nullptr};
    execv(bin.c_str(), argv);
    _exit(127);
  }
  close(pipeFd[0]);
  close(slaveFd);

  // Send the piped content and EOF.
  std::string payload = "line one\nline two\nline three\n";
  ::write(pipeFd[1], payload.data(), payload.size());
  close(pipeFd[1]);

  // Read PTY output until we see the seeded text.
  std::string result;
  char buf[4096];
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{3000};
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd pfd{};
    pfd.fd = masterFd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 200);
    if (ret <= 0)
      continue;
    if (pfd.revents & POLLIN) {
      ssize_t n = ::read(masterFd, buf, sizeof(buf));
      if (n <= 0)
        break;
      result.append(buf, n);
      if (result.find("line three") != std::string::npos)
        break;
    }
  }
  auto stripped = stripAnsi(result);
  BOOST_CHECK(stripped.find("line one") != std::string::npos);
  BOOST_CHECK(stripped.find("line three") != std::string::npos);

  // Send Esc-c to exit; child still has /dev/tty as input, so this works.
  ::write(masterFd,
          "\x1b"
          "c"
          "n",
          3);
  int status = 0;
  for (int i = 0; i < 200; ++i) {
    if (waitpid(pid, &status, WNOHANG) == pid)
      break;
    usleep(10000);
  }
  if (waitpid(pid, &status, WNOHANG) != pid)
    kill(pid, SIGKILL);
  close(masterFd);
}

BOOST_AUTO_TEST_CASE(e2e_open_file) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("test content here\nsecond line\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  auto output = session.readUntil("test content");
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("test content here") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_resize) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Resize to smaller terminal.
  session.resize(10, 40);
  auto output = session.readOutput(std::chrono::milliseconds{300});
  // Editor should have redrawn — output should not be empty.
  // (We can't easily verify dimensions, but at least it shouldn't crash.)
  BOOST_CHECK(!output.empty());

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_help_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Send Esc-? to toggle help.
  session.send("\x1b?");
  auto output = session.readUntil("Ctrl", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  // Help buffer should contain keybinding descriptions like "Ctrl".
  BOOST_CHECK(stripped.find("Ctrl") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_discard_without_saving) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Type some text so the buffer is modified.
  session.send("some text");
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-c to exit, then 'n' to discard unsaved changes.
  session.send("\x1b"
               "c"
               "n");
  int exitCode = session.waitExit();
  BOOST_CHECK_EQUAL(exitCode, 0);
}

BOOST_AUTO_TEST_CASE(e2e_exit_saveas_cancel_does_not_loop) {
  // Regression for B4: exit flow gets stuck when the user cancels the
  // SaveAs prompt opened from the kill-confirm prompt. Previously,
  // _exitPending was left set, so the main loop immediately re-entered
  // tryExit() and re-opened the kill-confirm prompt for the same buffer.
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Start without any file argument -> the initial buffer is pathless.
  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Type some text so the buffer is modified.
  session.send("hello");
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-c -> kill-confirm prompt; 'y' -> save first -> SaveAs prompt opens.
  session.send("\x1b"
               "c"
               "y");
  // Wait for the SaveAs prompt to render.
  auto saveAsOut = session.readUntil("Save as", std::chrono::milliseconds{500});
  BOOST_CHECK(stripAnsi(saveAsOut).find("Save as") != std::string::npos);

  // Esc to abort the SaveAs prompt. With the bug, this would immediately
  // re-trigger the kill-confirm prompt ("Exiting. Save temporary buffer?")
  // because _exitPending was left set; with the fix, we return to normal
  // editing with no prompt.
  session.send("\x1b");
  auto afterAbort = session.readOutput(std::chrono::milliseconds{400});
  auto strippedAbort = stripAnsi(afterAbort);
  BOOST_CHECK(strippedAbort.find("Save as") == std::string::npos);
  BOOST_CHECK(strippedAbort.find("Exiting") == std::string::npos);
  BOOST_CHECK(strippedAbort.find("Save temporary buffer") == std::string::npos);

  // Now we should be able to continue editing and discard via Esc-c + n.
  session.send(" world");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c"
               "n");
  int exitCode = session.waitExit();
  BOOST_CHECK_EQUAL(exitCode, 0);
}

BOOST_AUTO_TEST_CASE(e2e_find_basic) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("alpha beta gamma delta");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("alpha", std::chrono::milliseconds{500});

  // Ctrl-F to open find, type "gamma", Enter to search.
  session.send("\x06"
               "gamma\r");
  auto output = session.readOutput(std::chrono::milliseconds{500});
  auto stripped = stripAnsi(output);
  // The buffer should have found "gamma" — no "Not found" message.
  BOOST_CHECK(stripped.find("Not found") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_not_found) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("hello world");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("hello", std::chrono::milliseconds{500});

  // Search for something that doesn't exist.
  session.send("\x06"
               "zzz\r");
  auto output = session.readUntil("Not found", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Not found") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_next_with_ctrl_f) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a file with repeated words.
  TempFile tmp("foo bar foo baz foo");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("foo", std::chrono::milliseconds{500});

  // First Ctrl-F: search for "foo".
  session.send("\x06"
               "foo\r");
  session.readOutput(std::chrono::milliseconds{300});

  // Second Ctrl-F should advance to next occurrence and re-prompt.
  // Press Enter with empty input to keep searching for "foo".
  session.send("\x06\r");
  session.readOutput(std::chrono::milliseconds{300});

  // Third Ctrl-F to advance again.
  session.send("\x06\r");
  auto output = session.readOutput(std::chrono::milliseconds{300});
  auto stripped = stripAnsi(output);
  // Should eventually wrap around. Check no crash occurred.
  BOOST_CHECK(!stripped.empty() || output.empty()); // at least didn't hang

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_case_insensitive) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("Hello World");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("Hello", std::chrono::milliseconds{500});

  // Lowercase search should find uppercase match (case-insensitive).
  session.send("\x06"
               "hello\r");
  auto output = session.readOutput(std::chrono::milliseconds{500});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Not found") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_case_sensitive) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("hello world");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("hello", std::chrono::milliseconds{500});

  // Uppercase search should not match lowercase text.
  session.send("\x06"
               "Hello\r");
  auto output = session.readUntil("Not found", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Not found") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_skips_digits) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // "a8b" should be found by searching "ab" (digits ignored when not in pattern).
  TempFile tmp("a8b");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("a8b", std::chrono::milliseconds{500});

  session.send("\x06"
               "ab\r");
  auto output = session.readOutput(std::chrono::milliseconds{500});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Not found") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_skips_special) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // "a#b" should be found by searching "ab" (specials ignored when not in pattern).
  TempFile tmp("a#b");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("a#b", std::chrono::milliseconds{500});

  session.send("\x06"
               "ab\r");
  auto output = session.readOutput(std::chrono::milliseconds{500});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Not found") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_find_prompt_shows) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Ctrl-F should open the Find prompt.
  session.send("\x06");
  auto output = session.readUntil("Find:", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Find:") != std::string::npos);

  // Escape to cancel, then exit.
  session.send("\x1b\x1b");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_revert_prompt_shows) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("original content");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("original", std::chrono::milliseconds{500});

  // Esc-r should show a revert confirmation prompt.
  session.send("\x1b"
               "r");
  auto output = session.readUntil("revert", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("revert") != std::string::npos ||
              stripped.find("Revert") != std::string::npos ||
              stripped.find("y/n") != std::string::npos);

  // Press 'n' to cancel.
  session.send("n");
  session.readOutput(std::chrono::milliseconds{200});

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_revert_restores_content) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("original content");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("original", std::chrono::milliseconds{500});

  // Type something to modify the buffer.
  session.send("MODIFIED ");
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-r then 'y' to revert.
  session.send("\x1b"
               "r"
               "y");
  auto output = session.readUntil("original", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  // After revert, original content should be displayed.
  BOOST_CHECK(stripped.find("original") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_revert_scratch_buffer_error) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Start with no file (scratch buffer).
  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-r on a scratch buffer should show an error.
  session.send("\x1b"
               "r");
  auto output = session.readOutput(std::chrono::milliseconds{500});
  auto stripped = stripAnsi(output);
  // Should mention inability to revert (temporary/scratch buffer).
  BOOST_CHECK(stripped.find("emporary") != std::string::npos ||
              stripped.find("revert") != std::string::npos ||
              stripped.find("no path") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_unrecognised_shortcut_message) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Ctrl-z is not a bound shortcut (no Esc timing issues).
  session.send("\x1a"); // Ctrl-Z = 0x1A
  auto output = session.readUntil("not recognised", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("not recognised") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_cpp_file_shows_c_mode) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("int main() {}", ".cpp");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  auto output = session.readUntil("[c]", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("[c]") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_txt_file_shows_text_mode) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("hello world", ".txt");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  auto output = session.readUntil("[txt]", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("[txt]") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_set_mode_to_text) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("int x;", ".cpp");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("[c]", std::chrono::milliseconds{1000});

  // Esc-m, type "txt", Enter.
  session.send("\x1b"
               "m"
               "txt\r");
  auto output = session.readUntil("Mode: txt", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Mode: txt") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_set_mode_to_c) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("hello", ".txt");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("[txt]", std::chrono::milliseconds{1000});

  // Esc-m, type "c", Enter.
  session.send("\x1b"
               "m"
               "c\r");
  auto output = session.readUntil("Mode: c", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Mode: c") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_set_mode_invalid_shows_error) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-m, type "bogus", Enter.
  session.send("\x1b"
               "m"
               "bogus\r");
  auto output = session.readUntil("Unknown mode", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Unknown mode") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_switch_buffer_resets_prompt) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Open a file, then switch to a new scratch buffer via Ctrl-O.
  TempFile tmp("original file content", ".txt");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("original", std::chrono::milliseconds{500});

  // Ctrl-O opens the switch-buffer prompt. Type a new name and press Enter
  // to open a new (scratch) buffer.
  session.send("\x0f"       // Ctrl-O
               "newbuf\r"); // type name + Enter
  // Wait for the prompt to clear and the new buffer to appear.
  session.readOutput(std::chrono::milliseconds{300});

  // Now type some text — if the prompt wasn't reset, keystrokes would be
  // swallowed by the stale prompt handler instead of being inserted.
  session.send("hello after switch");
  auto output = session.readUntil("hello after switch", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("hello after switch") != std::string::npos);

  // The status bar should NOT show "Buffer:" prompt text (the old bug).
  BOOST_CHECK(stripped.find("Buffer:") == std::string::npos);

  // Clean exit.
  session.send("\x1b"
               "c"
               "n"); // discard unsaved
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_set_mode_prompt_shows) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // Esc-m should open the mode prompt.
  session.send("\x1b"
               "m");
  auto output = session.readUntil("Mode", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK(stripped.find("Mode") != std::string::npos);

  // Escape to cancel.
  session.send("\x1b\x1b");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c");
  session.waitExit();
}

// --- Directory buffer cursor tests (single and multi-panel) ---

// Helper: send N down-arrows with small delays between each.
static void sendDownArrows(PtySession& session, int n) {
  for (int i = 0; i < n; ++i) {
    session.send("\x1b[B");                             // Down arrow
    session.readOutput(std::chrono::milliseconds{100}); // drain render output
  }
}

BOOST_AUTO_TEST_CASE(e2e_dir_open_file_single_panel) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a directory with three files.
  TempDir dir({"aaa", "bbb", "ccc"});
  PtySession session(24, 80, bin, {std::string(dir.path())});
  session.readUntil("aaa", std::chrono::milliseconds{1000});

  // Listing order: .  ..  aaa  bbb  ccc  (indices 0-4).
  // Move down 3 times (. -> .. -> aaa -> bbb), then Enter.
  sendDownArrows(session, 3);
  session.send("\r");
  auto output = session.readUntil("bbb", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  // The modeline should show "bbb" as the opened file's name.
  BOOST_CHECK_MESSAGE(stripped.find("bbb") != std::string::npos,
                      "Expected 'bbb' in output after opening second file");

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_dir_open_file_split_top_panel) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a directory with three files.
  TempDir dir({"aaa", "bbb", "ccc"});
  PtySession session(24, 80, bin, {std::string(dir.path())});
  session.readUntil("aaa", std::chrono::milliseconds{1000});

  // Split horizontally: Esc-h. Both panels show the same dir buffer.
  session.send("\x1b"
               "h");
  session.readOutput(std::chrono::milliseconds{300});

  // Focus stays on top panel (panel 1). Move down to bbb and press Enter.
  sendDownArrows(session, 3);
  session.send("\r");
  auto output = session.readUntil("bbb", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK_MESSAGE(stripped.find("bbb") != std::string::npos,
                      "Expected 'bbb' in output after opening from top panel");

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_dir_open_file_split_bottom_panel) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a directory with three files.
  TempDir dir({"aaa", "bbb", "ccc"});
  PtySession session(24, 80, bin, {std::string(dir.path())});
  session.readUntil("aaa", std::chrono::milliseconds{1000});

  // Split horizontally and switch focus to bottom panel (panel 2).
  session.send("\x1b"
               "h");
  session.readOutput(std::chrono::milliseconds{300});
  session.send("\x1b"
               "2");
  session.readOutput(std::chrono::milliseconds{300});

  // Move down to ccc (4 down-arrows: . -> .. -> aaa -> bbb -> ccc).
  sendDownArrows(session, 4);
  session.send("\r");
  auto output = session.readUntil("ccc", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK_MESSAGE(stripped.find("ccc") != std::string::npos,
                      "Expected 'ccc' in output after opening from bottom panel");

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_dir_open_file_split_vertical_right) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a directory with three files.
  TempDir dir({"aaa", "bbb", "ccc"});
  PtySession session(24, 80, bin, {std::string(dir.path())});
  session.readUntil("aaa", std::chrono::milliseconds{1000});

  // Split vertically and switch focus to right panel (panel 2).
  session.send("\x1b"
               "v");
  session.readOutput(std::chrono::milliseconds{300});
  session.send("\x1b"
               "2");
  session.readOutput(std::chrono::milliseconds{300});

  // Move down to aaa (2 down-arrows: . -> .. -> aaa), then Enter.
  sendDownArrows(session, 2);
  session.send("\r");
  auto output = session.readUntil("aaa", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK_MESSAGE(stripped.find("aaa") != std::string::npos,
                      "Expected 'aaa' in output after opening from right panel");

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_dir_navigate_parent_in_split) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Create a nested directory structure.
  TempDir dir({"file1"});
  auto subdir = dir.path() / "subdir";
  std::filesystem::create_directories(subdir);
  std::ofstream(subdir / "inner") << "inner";

  // Open the subdir.
  PtySession session(24, 80, bin, {std::string(subdir)});
  session.readUntil("inner", std::chrono::milliseconds{1000});

  // Split horizontally and focus bottom panel.
  session.send("\x1b"
               "h");
  session.readOutput(std::chrono::milliseconds{300});
  session.send("\x1b"
               "2");
  session.readOutput(std::chrono::milliseconds{300});

  // Move down once (. -> ..) and press Enter to navigate to parent.
  sendDownArrows(session, 1);
  session.send("\r");
  // The parent directory should now be listed, containing "subdir" and "file1".
  auto output = session.readUntil("file1", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK_MESSAGE(stripped.find("file1") != std::string::npos,
                      "Expected parent dir contents after navigating to '..' in split panel");

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_kill_buffer_blocked_in_history_mode) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("original content\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("original", std::chrono::milliseconds{500});

  // Type text to create version history.
  session.send("added ");
  session.readOutput(std::chrono::milliseconds{300});

  // Enter history mode (Ctrl-U), then try to kill buffer (C-x k).
  session.send("\x15");
  auto histOut = session.readUntil("step", std::chrono::milliseconds{1000});
  auto histStripped = stripAnsi(histOut);
  BOOST_CHECK_MESSAGE(histStripped.find("step") != std::string::npos,
                      "Expected history modeline: " + histStripped);

  session.send("\x18k");
  auto output = session.readUntil("Exit history mode", std::chrono::milliseconds{1000});
  auto stripped = stripAnsi(output);
  BOOST_CHECK_MESSAGE(stripped.find("Exit history mode") != std::string::npos,
                      "Expected 'Exit history mode' in: " + stripped);

  // Exit history mode (Ctrl-U again) then quit.
  session.send("\x15");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_kill_buffer_repeatedly_survives) {
  // C-x k used to switch the focused panel via setBuffer(), which saved
  // view state into the just-destroyed buffer (use-after-free). Silent in a
  // plain build; glibc malloc perturbation makes the freed memory garbage
  // and a sanitizer build reports it deterministically.
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile a("alpha file\n"), b("bravo file\n"), c("charlie file\n");
  setenv("MALLOC_PERTURB_", "165", 1);
  PtySession session(24, 80, bin, {a.path().string(), b.path().string(), c.path().string()});
  unsetenv("MALLOC_PERTURB_");
  session.readOutput(std::chrono::milliseconds{300});

  // Split so the killed buffer is also visible in another panel, then kill
  // three buffers in a row.
  session.send("\x18v");
  session.readOutput(std::chrono::milliseconds{200});
  for (int i = 0; i < 3; ++i) {
    session.send("\x18k");
    session.readOutput(std::chrono::milliseconds{200});
  }
  session.send("still alive");
  auto out = stripAnsi(session.readUntil("still alive", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("still alive") != std::string::npos,
                      "Editor did not respond: " + out);

  session.send("\x1b"
               "c");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("n");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

BOOST_AUTO_TEST_CASE(e2e_save_as_onto_open_file_is_refused) {
  // Save-as onto a path already open in another buffer used to destroy the
  // focused buffer inside BufferManager::rename() (use-after-free).
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("on disk\n");
  auto path = std::filesystem::weakly_canonical(tmp.path()).string();
  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{300});

  // Open the file, switch back to the scratch buffer and type into it.
  session.send("\x0f" + path + "\r");
  session.readUntil("on disk", std::chrono::milliseconds{1000});
  session.send("\x0f*1*\r");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("scratch text");
  session.readUntil("scratch text", std::chrono::milliseconds{1000});

  // Save-as onto the open file and confirm the overwrite.
  session.send("\x13");
  session.readUntil("Save as", std::chrono::milliseconds{1000});
  session.send(path + "\r");
  session.readUntil("Overwrite", std::chrono::milliseconds{1000});
  session.send("y");
  auto out = stripAnsi(session.readUntil("Already open", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Already open") != std::string::npos, "Expected refusal: " + out);

  // The file on disk is untouched and the editor still works.
  std::ifstream f(tmp.path());
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  BOOST_CHECK_EQUAL(content, "on disk\n");
  session.send("!");
  out = stripAnsi(session.readUntil("scratch text!", std::chrono::milliseconds{1000}));
  BOOST_CHECK(out.find("scratch text!") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(e2e_filesystem_errors_do_not_end_session) {
  // Throwing std::filesystem calls on untraversable paths used to unwind
  // to main() and exit, dropping all unsaved work.
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"f.txt"});
  auto locked = dir.path() / "locked";
  std::filesystem::create_directory(locked);
  struct Restore {
    std::vector<std::filesystem::path> paths;
    ~Restore() {
      std::error_code ec;
      for (auto& p : paths)
        std::filesystem::permissions(p, std::filesystem::perms::owner_all, ec);
    }
  } restore{{locked, dir.path()}};
  std::filesystem::permissions(locked, std::filesystem::perms::none);
  if (::access((locked / ".").c_str(), X_OK) == 0)
    return; // privileges bypass permissions; nothing to test

  PtySession session(24, 80, bin, {(dir.path() / "f.txt").string()});
  session.readUntil("f.txt", std::chrono::milliseconds{1000});
  session.send("edited ");
  session.readUntil("edited", std::chrono::milliseconds{1000});

  // Open a file below the untraversable directory.
  session.send("\x0f" + (locked / "x.txt").string() + "\r");
  session.readOutput(std::chrono::milliseconds{300});
  BOOST_CHECK_EQUAL(session.waitExit(std::chrono::milliseconds{100}), -1);

  // Back to f.txt, then save after its directory lost its x bit.
  session.send("\x0f" + (dir.path() / "f.txt").string() + "\r");
  session.readUntil("edited", std::chrono::milliseconds{1000});
  std::filesystem::permissions(dir.path(), std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_write);
  session.send("\x13");
  session.readOutput(std::chrono::milliseconds{300});
  std::filesystem::permissions(dir.path(), std::filesystem::perms::owner_all);
  BOOST_CHECK_EQUAL(session.waitExit(std::chrono::milliseconds{100}), -1);

  // Still alive and editable; a second save now succeeds.
  session.send("\x13");
  auto out = stripAnsi(session.readUntil("Saved", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Saved") != std::string::npos, "Expected save: " + out);
}

BOOST_AUTO_TEST_CASE(e2e_regex_find_next_advances_past_empty_match) {
  // The cursor lands at the match end, which for an empty match is its
  // start: the next search used to find the same match forever.
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("ab\ncd\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("ab", std::chrono::milliseconds{1000});

  session.send("\x1bX");
  session.readUntil("Regex find", std::chrono::milliseconds{1000});
  session.send("x*\r");
  auto out = stripAnsi(session.readUntil("1:2", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("1:2") != std::string::npos, "Expected 1:2 in: " + out);
  session.send("\x1bX");
  out = stripAnsi(session.readUntil("1:3", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("1:3") != std::string::npos, "Expected 1:3 in: " + out);
}

BOOST_AUTO_TEST_CASE(e2e_regex_replace_skip_and_all_with_empty_matches) {
  // Skipping (n) an empty match re-prompted at the same spot forever, and
  // replace-all (!) looped until memory ran out.
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("ab\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("ab", std::chrono::milliseconds{1000});

  // Skip every empty match of x*: positions 1:1, 1:2, 1:3 and 2:1.
  session.send("\x1bY");
  session.readUntil("Regex pattern", std::chrono::milliseconds{1000});
  session.send("x*\r");
  session.readUntil("with", std::chrono::milliseconds{1000});
  session.send("-\r");
  session.readUntil("Replace this", std::chrono::milliseconds{1000});
  for (int i = 0; i < 3; ++i) {
    session.send("n");
    session.readUntil("Replace this", std::chrono::milliseconds{1000});
  }
  session.send("n");
  auto out = stripAnsi(session.readUntil("Replaced 0", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Replaced 0") != std::string::npos, "Expected end of scan: " + out);

  // Replace all word boundaries (the scan starts at the cursor, so use a
  // fresh session).
  PtySession session2(24, 80, bin, {tmp.path().string()});
  session2.readUntil("ab", std::chrono::milliseconds{1000});
  session2.send("\x1bY");
  session2.readUntil("Regex pattern", std::chrono::milliseconds{1000});
  session2.send("\\b\r");
  session2.readUntil("with", std::chrono::milliseconds{1000});
  session2.send("xx\r");
  session2.readUntil("Replace this", std::chrono::milliseconds{1000});
  session2.send("!");
  out = stripAnsi(session2.readUntil("Replaced 2", std::chrono::milliseconds{2000}));
  BOOST_CHECK_MESSAGE(out.find("Replaced 2") != std::string::npos,
                      "Expected 2 replacements: " + out);
  BOOST_CHECK(out.find("xxabxx") != std::string::npos);
}

// --- Bookmarks (C-x b / C-x j) ---

BOOST_AUTO_TEST_CASE(e2e_bookmark_set_and_jump_returns_to_position) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("line1\nline2\nline3\nline4\nline5\nline6\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("line1", std::chrono::milliseconds{500});

  // Move down to physical line 4 (1-based "4:1") and bookmark it.
  sendDownArrows(session, 3);
  session.readUntil("4:1", std::chrono::milliseconds{1000});
  session.send("\x18"
               "b"
               "a");
  auto setOut = session.readUntil("Bookmark 'a' set", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(setOut).find("Bookmark 'a' set") != std::string::npos);

  // Move further away from the bookmark. (Not using sendDownArrows here:
  // it drains output after each keypress, which would swallow the "6:1"
  // redraw before the readUntil below gets a chance to see it.)
  session.send("\x1b[B\x1b[B");
  auto movedOut = session.readUntil("6:1", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(movedOut).find("6:1") != std::string::npos);

  // Jump back — the cursor should return to "4:1".
  session.send("\x18j"
               "a");
  auto jumpOut = session.readUntil("4:1", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(jumpOut).find("4:1") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_jump_switches_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile a("AAAAA content\n", ".txt");
  TempFile b("BBBBB content\n", ".txt");
  PtySession session(24, 80, bin, {std::string(a.path())});
  session.readUntil("AAAAA content", std::chrono::milliseconds{500});

  // Bookmark the starting position in buffer A.
  session.send("\x18"
               "b"
               "a");
  session.readUntil("Bookmark 'a' set", std::chrono::milliseconds{1000});

  // Switch to buffer B via Ctrl-O.
  session.send("\x0f" + b.path().string() + "\r");
  auto switchedOut = session.readUntil("BBBBB content", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(switchedOut).find("BBBBB content") != std::string::npos);

  // Jumping to the bookmark should switch the panel back to buffer A.
  session.send("\x18j"
               "a");
  auto jumpOut = session.readUntil("AAAAA content", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(jumpOut).find("AAAAA content") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_jump_clamps_after_lines_deleted) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Six physical lines (five content lines plus the trailing empty entry
  // for the file's final '\n').
  TempFile tmp("line1\nline2\nline3\nline4\nline5\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("line1", std::chrono::milliseconds{500});

  // Bookmark the last content line ("5:1", 0-indexed line 4).
  sendDownArrows(session, 4);
  session.readUntil("5:1", std::chrono::milliseconds{1000});
  session.send("\x18"
               "b"
               "a");
  session.readUntil("Bookmark 'a' set", std::chrono::milliseconds{1000});

  // Move back to the top, then delete the first four lines (Ctrl-K),
  // leaving only "line5" and the trailing empty line — the bookmarked
  // index (4) no longer exists. Sent as one batch (rather than via
  // sendDownArrows/readOutput per key) so nothing drains the "line5"
  // redraw before the readUntil below gets to see it.
  session.send("\x1b[A\x1b[A\x1b[A\x1b[A");
  session.send("\x0b\x0b\x0b\x0b"); // four Ctrl-K (DeleteLine)
  auto afterDelete = session.readUntil("line5", std::chrono::milliseconds{1500});
  BOOST_CHECK(stripAnsi(afterDelete).find("line5") != std::string::npos);

  // Jump to the bookmark — must clamp to the new last line ("2:1") rather
  // than crash or run off the end of the shrunk buffer.
  session.send("\x18j"
               "a");
  auto jumpOut = session.readUntil("2:1", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(jumpOut).find("2:1") != std::string::npos);

  // Editor must still be responsive after the clamp.
  session.send("still alive");
  auto typedOut = session.readUntil("still alive", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(typedOut).find("still alive") != std::string::npos);

  session.send("\x1b"
               "c"
               "n"); // discard unsaved edit
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_jump_to_killed_buffer_drops_slot) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile a("doomed buffer content\n", ".txt");
  PtySession session(24, 80, bin, {std::string(a.path())});
  session.readUntil("doomed buffer", std::chrono::milliseconds{500});

  // Bookmark 'a' in the file buffer, which will be killed below.
  session.send("\x18"
               "b"
               "a");
  session.readUntil("Bookmark 'a' set", std::chrono::milliseconds{1000});

  // Switch to a new scratch buffer and bookmark it as 'z' so the store
  // isn't empty once 'a' is dropped.
  session.send("\x0fscratch\r");
  session.readUntil("scratch", std::chrono::milliseconds{1000});
  session.send("\x18"
               "b"
               "z");
  session.readUntil("Bookmark 'z' set", std::chrono::milliseconds{1000});

  // Switch back to the file buffer and kill it (unmodified, so no confirm
  // prompt is shown).
  session.send("\x0f" + a.path().string() + "\r");
  session.readUntil("doomed buffer", std::chrono::milliseconds{1000});
  session.send("\x18k");
  session.readOutput(std::chrono::milliseconds{300});

  // First jump: the bookmarked buffer is gone — status reports it and
  // drops the slot.
  session.send("\x18j"
               "a");
  auto goneOut = session.readUntil("Bookmark 'a': buffer gone", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(goneOut).find("Bookmark 'a': buffer gone") != std::string::npos);

  // Second jump: slot 'a' is now empty ('z' still occupied, so the prompt
  // still opens rather than short-circuiting to "No bookmarks set").
  session.send("\x18j"
               "a");
  auto emptyOut = session.readUntil("No bookmark 'a'", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(emptyOut).find("No bookmark 'a'") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_jump_with_none_set_shows_status) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  session.send("\x18j");
  auto output = session.readUntil("No bookmarks set", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(output).find("No bookmarks set") != std::string::npos);

  // Editor must still be responsive — no prompt should have opened.
  session.send("still here");
  auto typedOut = session.readUntil("still here", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(typedOut).find("still here") != std::string::npos);

  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_refused_in_help_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  // *help* is a singleton overlay held outside BufferManager's maps, so
  // find() can never resolve it. Setting a bookmark there used to appear
  // to succeed and then silently erase the slot on the first jump.
  session.send("\x1b?");
  session.readUntil("Show this help", std::chrono::milliseconds{1000});
  session.send("\x18"
               "b"
               "a");
  auto refusedOut =
      session.readUntil("Cannot bookmark this buffer", std::chrono::milliseconds{1000});
  auto strippedRefused = stripAnsi(refusedOut);
  BOOST_CHECK(strippedRefused.find("Cannot bookmark this buffer") != std::string::npos);
  BOOST_CHECK(strippedRefused.find("Bookmark 'a' set") == std::string::npos);

  // Leave help; no slot should have been recorded, so the jump prompt
  // short-circuits rather than opening and reporting "buffer gone".
  session.send("q");
  session.readOutput(std::chrono::milliseconds{300});
  session.send("\x18j");
  auto jumpOut = session.readUntil("No bookmarks set", std::chrono::milliseconds{1000});
  auto strippedJump = stripAnsi(jumpOut);
  BOOST_CHECK(strippedJump.find("No bookmarks set") != std::string::npos);
  BOOST_CHECK(strippedJump.find("buffer gone") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_bookmark_survives_save_as_rename) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Start pathless ("*1*") so Ctrl-S opens the Save As prompt directly.
  PtySession session(24, 80, bin);
  session.readOutput(std::chrono::milliseconds{200});

  session.send("hello world");
  session.readUntil("hello world", std::chrono::milliseconds{1000});

  // Bookmark the current position in the pathless buffer.
  session.send("\x18"
               "b"
               "a");
  session.readUntil("Bookmark 'a' set", std::chrono::milliseconds{1000});

  // Save As to a path that does not yet exist, so no overwrite-confirm
  // step is needed.
  auto newPath = std::filesystem::temp_directory_path() /
                 ("cx_e2e_bookmark_saveas_" + std::to_string(::getpid()) + ".txt");
  std::filesystem::remove(newPath); // just in case a stale run left it behind
  session.send("\x13");             // Ctrl-S
  session.readUntil("Save as", std::chrono::milliseconds{1000});
  session.send(newPath.string() + "\r");
  auto savedOut = session.readUntil("Saved to", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(savedOut).find("Saved to") != std::string::npos);

  // Switch away to a scratch buffer.
  session.send("\x0fotherbuf\r");
  session.readUntil("otherbuf", std::chrono::milliseconds{1000});

  // Jump back: if the rename wasn't re-keyed into the bookmark store,
  // this would report "buffer gone" (still pointing at the old "*1*"
  // name, which no longer exists after the rename).
  session.send("\x18j"
               "a");
  auto jumpOut = session.readUntil("hello world", std::chrono::milliseconds{1000});
  auto strippedJump = stripAnsi(jumpOut);
  BOOST_CHECK(strippedJump.find("hello world") != std::string::npos);
  BOOST_CHECK(strippedJump.find("buffer gone") == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
  std::filesystem::remove(newPath);
}

BOOST_AUTO_TEST_CASE(e2e_select_all_occurrences_then_retype) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("foo one\ntwo foo\nfoo three\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("foo one", std::chrono::milliseconds{500});

  // Esc-* puts a cursor after every "foo"; the modeline shows the count.
  session.send("\x1b*");
  auto starOut = session.readUntil("Created 3 cursors", std::chrono::milliseconds{1000});
  auto starPlain = stripAnsi(starOut);
  BOOST_CHECK(starPlain.find("Created 3 cursors") != std::string::npos);
  // The running count lives in the modeline indicator, not the status.
  BOOST_CHECK(starPlain.find("3@I") != std::string::npos);

  // Ctrl-Bksp kills the word at every cursor, then typing replaces it.
  session.send("\x08");
  session.send("bar");
  auto typedOut = session.readUntil("bar three", std::chrono::milliseconds{1000});
  auto plain = stripAnsi(typedOut);
  BOOST_CHECK(plain.find("bar one") != std::string::npos);
  BOOST_CHECK(plain.find("two bar") != std::string::npos);
  BOOST_CHECK(plain.find("bar three") != std::string::npos);

  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_add_cursor_down_edits_several_lines) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("aaa\nbbb\nccc\nddd\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("aaa", std::chrono::milliseconds{500});

  // Ctrl-Shift-Down twice: three cursors, on lines 1-3 at column 0.
  session.send("\x1b[1;6B\x1b[1;6B");
  auto addOut = session.readUntil("3@I", std::chrono::milliseconds{1000});
  BOOST_CHECK(stripAnsi(addOut).find("3@I") != std::string::npos);

  session.send(">");
  auto typedOut = session.readUntil(">ccc", std::chrono::milliseconds{1000});
  auto plain = stripAnsi(typedOut);
  BOOST_CHECK(plain.find(">aaa") != std::string::npos);
  BOOST_CHECK(plain.find(">bbb") != std::string::npos);
  BOOST_CHECK(plain.find(">ccc") != std::string::npos);
  // The fourth line had no cursor and must be untouched.
  BOOST_CHECK(plain.find(">ddd") == std::string::npos);

  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_esc_esc_returns_to_a_single_cursor) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("aaa\nbbb\nccc\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("aaa", std::chrono::milliseconds{500});

  session.send("\x1b[1;6B");
  session.readUntil("2@I", std::chrono::milliseconds{1000});

  session.send("\x1b\x1b");
  auto collapsed = session.readUntil("Single cursor", std::chrono::milliseconds{1000});
  auto collapsedPlain = stripAnsi(collapsed);
  BOOST_CHECK(collapsedPlain.find("Single cursor") != std::string::npos);
  // The count field blanks out again, keeping the indicator's width.
  BOOST_CHECK(collapsedPlain.find("@I") == std::string::npos);

  // Only the primary line is edited now.
  session.send("X");
  auto typedOut = session.readUntil("Xaaa", std::chrono::milliseconds{1000});
  auto plain = stripAnsi(typedOut);
  BOOST_CHECK(plain.find("Xaaa") != std::string::npos);
  BOOST_CHECK(plain.find("Xbbb") == std::string::npos);

  session.send("\x1b"
               "c"
               "n");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_add_cursor_reachable_by_name_when_the_chord_is_swallowed) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("aaa\nbbb\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("aaa", std::chrono::milliseconds{500});

  // Esc-x by name is the fallback when a terminal eats Ctrl-Shift-Down.
  session.send("\x1bx"
               "Add cursor on line below\r");
  auto out = session.readUntil("2@I", std::chrono::milliseconds{1000});
  auto plain = stripAnsi(out);
  BOOST_CHECK(plain.find("2@I") != std::string::npos);
  // The chordless action must not splat a NUL binding hint into the modeline.
  BOOST_CHECK(out.find('\0') == std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_cursor_count_indicator_saturates_above_999) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // 1000 occurrences: one more than the three-digit field can show.
  std::string content;
  for (int i = 0; i < 1000; ++i)
    content += "foo\n";
  TempFile tmp(content);
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("foo", std::chrono::milliseconds{500});

  session.send("\x1b*");
  auto out = session.readUntil("***@I", std::chrono::milliseconds{2000});
  BOOST_CHECK(stripAnsi(out).find("***@I") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

// Grep runs a real subprocess, so these cover the pieces the GrepBuffer
// unit tests cannot: backend selection, argument construction, and the
// parse of what the backend actually wrote.
BOOST_AUTO_TEST_CASE(e2e_grep_lists_matches) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // TempDir writes each filename as that file's content, so searching for
  // "beta" matches beta.txt and nothing else.
  TempDir dir({"alpha.txt", "beta.txt"});
  PtySession session(24, 80, bin, {(dir.path() / "alpha.txt").string()});
  session.readUntil("alpha", std::chrono::milliseconds{500});

  session.send("\x1b"
               "gbeta\r");
  auto out = session.readUntil("1 match", std::chrono::milliseconds{5000});
  auto stripped = stripAnsi(out);
  BOOST_CHECK(stripped.find("1 match") != std::string::npos);
  BOOST_CHECK(stripped.find("beta.txt:1:") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

// Grepping '.' over a 4M-line file ran out of memory: the search now stops
// at 100,000 result lines and says so.
BOOST_AUTO_TEST_CASE(e2e_grep_stops_at_the_hit_limit) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"alpha.txt"});
  {
    std::ofstream big(dir.path() / "big.txt");
    for (int i = 0; i < 150'000; ++i)
      big << "hit\n";
  }
  PtySession session(24, 80, bin, {(dir.path() / "alpha.txt").string()});
  session.readUntil("alpha", std::chrono::milliseconds{500});

  session.send("\x1b"
               "ghit\r");
  auto out = stripAnsi(session.readUntil("limit)", std::chrono::milliseconds{10000}));
  BOOST_CHECK_MESSAGE(out.find("100000 matches (search stopped at the limit)") != std::string::npos,
                      "Expected the limit in the status: " + out);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_grep_reports_no_matches) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"alpha.txt"});
  PtySession session(24, 80, bin, {(dir.path() / "alpha.txt").string()});
  session.readUntil("alpha", std::chrono::milliseconds{500});

  session.send("\x1b"
               "gzzzznope\r");
  auto out = session.readUntil("No matches", std::chrono::milliseconds{5000});
  BOOST_CHECK(stripAnsi(out).find("No matches") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_grep_enter_opens_hit_beside_results) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"alpha.txt", "beta.txt"});
  PtySession session(24, 80, bin, {(dir.path() / "alpha.txt").string()});
  session.readUntil("alpha", std::chrono::milliseconds{500});

  session.send("\x1b"
               "gbeta\r");
  session.readUntil("beta.txt:1:", std::chrono::milliseconds{5000});

  // Enter opens the hit. Only one panel was open, so cx splits first —
  // the result listing must still be on screen afterwards.
  session.send("\r");
  auto out = session.readUntil("beta.txt", std::chrono::milliseconds{2000});
  auto stripped = stripAnsi(out);
  BOOST_CHECK(stripped.find("beta.txt:1:") != std::string::npos);
  BOOST_CHECK(stripped.find("*grep*") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_grep_in_repo_searches_from_root) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }
  if (std::system("git --version >/dev/null 2>&1") != 0) {
    BOOST_WARN_MESSAGE(false, "git not available, skipping repo grep test");
    return;
  }

  // A repo whose match lives at the ROOT, while the open buffer sits in a
  // subdirectory. A search anchored at the buffer's own directory would
  // miss it, so finding it proves we searched from the top level.
  TempDir dir;
  std::filesystem::create_directories(dir.path() / "sub");
  {
    std::ofstream root(dir.path() / "target.txt");
    root << "the needle is here\n";
    std::ofstream leaf(dir.path() / "sub" / "open_me.txt");
    leaf << "unrelated content\n";
  }
  auto git = "git -C " + dir.path().string() + " ";
  BOOST_REQUIRE_EQUAL(std::system((git + "init -q").c_str()), 0);
  BOOST_REQUIRE_EQUAL(std::system((git + "add -A").c_str()), 0);

  PtySession session(24, 80, bin, {(dir.path() / "sub" / "open_me.txt").string()});
  session.readUntil("unrelated", std::chrono::milliseconds{500});

  session.send("\x1b"
               "gneedle\r");
  auto out = session.readUntil("target.txt", std::chrono::milliseconds{5000});
  auto stripped = stripAnsi(out);
  BOOST_CHECK(stripped.find("1 match") != std::string::npos);
  BOOST_CHECK(stripped.find("target.txt:1:") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_grep_hit_lands_mid_panel) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir;
  {
    std::ofstream f(dir.path() / "long.txt");
    for (int i = 1; i <= 200; ++i)
      f << "line" << std::setw(3) << std::setfill('0') << i << "\n";
    std::ofstream open(dir.path() / "open_me.txt");
    open << "start here\n";
  }

  PtySession session(24, 80, bin, {(dir.path() / "open_me.txt").string()});
  session.readUntil("start here", std::chrono::milliseconds{500});

  session.send("\x1b"
               "gline100\r");
  session.readUntil("long.txt:100:", std::chrono::milliseconds{5000});
  session.send("\r");
  // Wait on context *below* the match, not on the match itself: the grep
  // panel still shows "line100" in the hit text, so waiting for that
  // returns a frame from before the file panel has redrawn.
  auto stripped = stripAnsi(session.readUntil("line105", std::chrono::milliseconds{2000}));

  // Centred, so lines well after the match are on screen too. Pinned to
  // the bottom row (the old ensureVisible behaviour) nothing past
  // line100 was drawn at all.
  BOOST_CHECK(stripped.find("line100") != std::string::npos);
  BOOST_CHECK(stripped.find("line105") != std::string::npos);
  // ...and context before it survives as well.
  BOOST_CHECK(stripped.find("line095") != std::string::npos);

  session.send("\x1b"
               "c");
  session.waitExit();
}

BOOST_AUTO_TEST_CASE(e2e_prompt_cursor_movement_and_display) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  // Start pathless ("*1*") so Ctrl-S opens the Save As prompt directly.
  PtySession session(24, 120, bin);
  session.readOutput(std::chrono::milliseconds{200});
  session.send("x");
  session.readUntil("x", std::chrono::milliseconds{1000});

  auto dir = std::filesystem::temp_directory_path();
  auto stem = "cx_e2e_prompt_cursor_" + std::to_string(::getpid()) + "_";
  auto wantPath = dir / (stem + "ab.txt");
  std::filesystem::remove(wantPath);

  session.send("\x13"); // Ctrl-S
  session.readUntil("Save as", std::chrono::milliseconds{1000});
  // Type the name without the 'a', walk back over "b.txt" and insert it.
  // Ctrl-Left / Ctrl-Right / Home / End must be consumed whole — a leaked
  // ";5D" tail would end up in the filename.
  session.send((dir / (stem + "b.txt")).string());
  session.send("\x1b[D\x1b[D\x1b[D\x1b[D\x1b[D");
  session.send("a");
  session.send("\x1b[1;5D\x1b[1;5C\x1b[H\x1b[F");
  auto out = session.readOutput(std::chrono::milliseconds{300});
  // The cursor cell is drawn in normal video inside the reverse-video bar,
  // with the terminal cursor hidden.
  BOOST_CHECK(out.find("\x1b[27m") != std::string::npos);
  BOOST_CHECK(out.find("\x1b[?25l") != std::string::npos);

  session.send("\r");
  session.readUntil("Saved to", std::chrono::milliseconds{1000});
  BOOST_CHECK(std::filesystem::exists(wantPath));

  session.send("\x1b"
               "c");
  session.waitExit();
  std::filesystem::remove(wantPath);
}

BOOST_AUTO_TEST_CASE(e2e_prompt_selection_cut_copy_paste) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 120, bin);
  session.readOutput(std::chrono::milliseconds{200});
  session.send("x");
  session.readUntil("x", std::chrono::milliseconds{1000});

  auto dir = std::filesystem::temp_directory_path();
  auto stem = "cx_e2e_prompt_select_" + std::to_string(::getpid()) + "_";
  auto wantPath = dir / (stem + "abab");
  std::filesystem::remove(wantPath);

  constexpr std::string_view ctrlSpace{"\0", 1};
  session.send("\x13"); // Ctrl-S
  session.readUntil("Save as", std::chrono::milliseconds{1000});
  session.send((dir / (stem + "ab.txt")).string());

  // Select "ab" (mark before ".txt", two Lefts); the region is drawn
  // underlined in normal video.
  session.send("\x1b[D\x1b[D\x1b[D\x1b[D");
  session.send(ctrlSpace);
  session.send("\x1b[D\x1b[D");
  auto selOut = session.readOutput(std::chrono::milliseconds{300});
  BOOST_CHECK(selOut.find("\x1b[4m") != std::string::npos);

  // Esc-w copies "ab"; then select ".txt" and cut it with Ctrl-W.
  session.send("\x1bw");
  session.send("\x1b[F");
  session.send(ctrlSpace);
  session.send("\x1b[D\x1b[D\x1b[D\x1b[D");
  session.send("\x17");
  // Ctrl-Y pastes the newest entry (".txt"), Esc-y cycles it to "ab".
  session.send("\x19");
  session.send("\x1by");
  session.readOutput(std::chrono::milliseconds{300});

  session.send("\r");
  session.readUntil("Saved to", std::chrono::milliseconds{1000});
  BOOST_CHECK(std::filesystem::exists(wantPath));

  session.send("\x1b"
               "c");
  session.waitExit();
  std::filesystem::remove(wantPath);
}

// CPU seconds (user + system) the process has used, from /proc/<pid>/stat.
static double cpuSeconds(pid_t pid) {
  std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
  std::string stat((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  auto pos = stat.rfind(')'); // the command name may contain spaces
  if (pos == std::string::npos)
    return 0;
  std::istringstream fields(stat.substr(pos + 1));
  std::string skip;
  for (int i = 0; i < 11; ++i) // state .. cmajflt
    fields >> skip;
  unsigned long utime = 0, stime = 0;
  fields >> utime >> stime;
  return static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
}

// A piped command that closes one of its outputs and keeps running
// (`cmd 2>/dev/null`) left that pipe at EOF in the poll set: poll returned
// at once every time, so the editor spun at 100% CPU and never reached
// the timeout that offers to kill the command. With both outputs closed
// the editor sat in a blocking waitpid() instead; now it polls for the
// child's exit, which must not spin either.
BOOST_AUTO_TEST_CASE(e2e_pipe_command_with_closed_outputs_does_not_spin) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  for (std::string command : {"exec sleep 3 2>/dev/null", "exec sleep 3 >/dev/null 2>&1"}) {
    BOOST_TEST_CONTEXT(command) {
      TempFile tmp("b\na\n");
      PtySession session(24, 80, bin, {std::string(tmp.path())});
      session.readOutput(std::chrono::milliseconds{300});

      // Mark, two lines down, Ctrl-P, command.
      session.send(std::string_view("\x00\x1b[B\x1b[B\x10", 8));
      session.send(command + "\r");
      session.readOutput(std::chrono::milliseconds{300});

      auto before = cpuSeconds(session.childPid);
      session.readOutput(std::chrono::milliseconds{1500});
      auto used = cpuSeconds(session.childPid) - before;
      BOOST_CHECK_LT(used, 0.5);

      // The command finishes on its own and the editor is usable again.
      session.readOutput(std::chrono::milliseconds{2000});
      session.send("\x1b"
                   "c"
                   "n");
      BOOST_CHECK_EQUAL(session.waitExit(), 0);
    }
  }
}

// A prompt scrolls its input so the cursor stays visible. Finding where
// to start used to re-measure the whole remainder for every character
// dropped from the front: pasting a 50 KB line into a prompt froze the
// editor for about ten seconds.
BOOST_AUTO_TEST_CASE(e2e_long_paste_into_prompt_is_fast_and_scrolled) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp(std::string(200000, 'x') + "END");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readOutput(std::chrono::milliseconds{300});

  // Mark, End, copy the line; Go-to-line prompt; paste.
  session.send(std::string_view("\x00\x1b[F\x1bw", 6));
  session.readOutput(std::chrono::milliseconds{300});
  auto before = cpuSeconds(session.childPid);
  session.send("\x07\x19");
  // The tail of the input is shown behind an ellipsis.
  auto output = session.readUntil("\xe2\x80\xa6", std::chrono::milliseconds{5000});
  output += session.readOutput(std::chrono::milliseconds{200});
  auto used = cpuSeconds(session.childPid) - before;
  BOOST_CHECK_LT(used, 1.0);
  BOOST_CHECK(output.find("\xe2\x80\xa6") != std::string::npos);
  BOOST_CHECK(stripAnsi(output).find("xxxxEND") != std::string::npos);

  session.send("\x1b");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c");
  session.waitExit();
}

// The "Replace '<pattern>' with:" labels wrote a pasted pattern to the
// terminal raw, so U+009B (C1 CSI) started an escape sequence and U+202E
// reversed the rest of the modeline. Both must be shown as escapes.
BOOST_AUTO_TEST_CASE(e2e_replace_label_escapes_control_characters) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("a\xc2\x9b"
               "31mb\xe2\x80\xae"
               "c\n");
  for (std::string_view key : {"\x1b%", "\x1bY"}) {
    PtySession session(24, 80, bin, {std::string(tmp.path())});
    session.readOutput(std::chrono::milliseconds{300});
    // Mark, End, copy the line; open the replace prompt; paste; Enter.
    session.send(std::string_view("\x00\x1b[F\x1bw", 6));
    session.readOutput(std::chrono::milliseconds{200});
    session.send(key);
    session.readOutput(std::chrono::milliseconds{200});
    session.send("\x19\r");
    auto output = session.readUntil("with:", std::chrono::milliseconds{1000});
    output += session.readOutput(std::chrono::milliseconds{100});
    auto at = output.rfind("Replace");
    BOOST_REQUIRE_MESSAGE(at != std::string::npos, "no replace label: " + stripAnsi(output));
    auto label = output.substr(at);
    BOOST_CHECK_MESSAGE(label.find("\xc2\x9b") == std::string::npos, "raw U+009B: " + label);
    BOOST_CHECK_MESSAGE(label.find("\xe2\x80\xae") == std::string::npos, "raw U+202E: " + label);
    auto plain = stripAnsi(label);
    BOOST_CHECK_MESSAGE(plain.find("a\\u009b31mb\\u202ec") != std::string::npos,
                        "escaped label: " + plain);

    session.send("\x1b");
    session.readOutput(std::chrono::milliseconds{200});
    session.send("\x1b"
                 "c");
    session.waitExit();
  }
}

// Whole content of the file at @p path.
static std::string readFile(std::filesystem::path const& path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Help and README promise that Esc-Esc leaves history mode without
// committing and that Enter commits; neither key did anything, so every
// way out of history mode dropped the newer versions.
BOOST_AUTO_TEST_CASE(e2e_history_esc_esc_cancels_and_enter_commits) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  struct Case {
    std::string_view key, status, saved;
  };
  for (auto const& c : {Case{"\x1b\x1b", "History: cancelled", "abcbase\n"},
                        Case{"\r", "History: committed", "abase\n"}}) {
    BOOST_TEST_CONTEXT(c.status) {
      TempFile tmp("base\n");
      PtySession session(24, 80, bin, {std::string(tmp.path())});
      session.readUntil("base", std::chrono::milliseconds{500});
      session.send("abc");
      session.readOutput(std::chrono::milliseconds{200});
      // History mode, two versions back: the buffer shows "abase".
      session.send("\x15[[");
      session.readUntil("v 2/4", std::chrono::milliseconds{1000});

      session.send(c.key);
      auto out = stripAnsi(session.readUntil(c.status, std::chrono::milliseconds{1000}));
      BOOST_CHECK_MESSAGE(out.find(c.status) != std::string::npos, "status: " + out);

      session.send("\x13");
      session.readUntil("Saved", std::chrono::milliseconds{1000});
      BOOST_CHECK_EQUAL(readFile(tmp.path()), c.saved);
      session.send("\x1b"
                   "c");
      BOOST_CHECK_EQUAL(session.waitExit(), 0);
    }
  }
}

// Switching buffers drops the view, and used to commit the version it was
// browsing: looking at an older version and then opening the help screen
// silently discarded every newer version.
BOOST_AUTO_TEST_CASE(e2e_help_while_browsing_history_keeps_newer_versions) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("base\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("base", std::chrono::milliseconds{500});
  session.send("abc");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x15[[");
  session.readUntil("v 2/4", std::chrono::milliseconds{1000});

  // Help and back.
  session.send("\x1b?");
  session.readUntil("Press 'q'", std::chrono::milliseconds{1000});
  session.send("q");
  auto out = stripAnsi(session.readUntil("abcbase", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("abcbase") != std::string::npos, "newest version shown: " + out);

  // All four versions are still there.
  session.send("\x15");
  out = stripAnsi(session.readUntil("v 4/4", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("v 4/4") != std::string::npos, "history badge: " + out);

  session.send("\x15\x1b"
               "cn");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// Closing the panel that was browsing history dropped its view without
// leaving history mode. The buffer then stayed read-only on the older
// version for good: no typing, no save, no kill.
BOOST_AUTO_TEST_CASE(e2e_closing_the_panel_browsing_history_releases_the_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("base\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("base", std::chrono::milliseconds{500});
  session.send("abc");
  session.readOutput(std::chrono::milliseconds{200});
  // Split, browse one version back in the focused panel, close that panel.
  session.send("\x18v");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x15[");
  session.readUntil("v 3/4", std::chrono::milliseconds{1000});
  session.send("\x18.");
  session.readOutput(std::chrono::milliseconds{200});

  // The remaining panel shows the newest version and can edit and save it.
  session.send("Z");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x13");
  auto out = stripAnsi(session.readUntil("Saved", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Saved") != std::string::npos, "save status: " + out);
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "Zabcbase\n");

  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// Exit only asks about buffers that are not read-only, and a buffer is
// read-only while its history is browsed: Esc-c in history mode used to
// quit at once and lose the unsaved changes.
BOOST_AUTO_TEST_CASE(e2e_exit_while_browsing_history_asks_to_save) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("base\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("base", std::chrono::milliseconds{500});
  session.send("abc");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x15[");
  session.readUntil("v 3/4", std::chrono::milliseconds{1000});

  session.send("\x1b"
               "c");
  auto out = stripAnsi(session.readUntil("Exiting. Save", std::chrono::milliseconds{1000}));
  BOOST_REQUIRE_MESSAGE(out.find("Exiting. Save") != std::string::npos, "no save prompt: " + out);
  // What gets saved is the newest version, not the one that was shown.
  session.send("y");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "abcbase\n");
}

// The region was written to the command's stdin with one blocking write.
// A command that streams (cat, tr, sed) stops reading once its own output
// pipe is full, and nothing drained that pipe while the write was blocked:
// from about 1 MB on the editor hung for good, without the kill prompt.
BOOST_AUTO_TEST_CASE(e2e_pipe_large_region_through_streaming_command) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  std::string body;
  for (int i = 0; i < 20000; ++i)
    body += std::string(99, 'x') + "\n";
  TempFile tmp(body);
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readOutput(std::chrono::milliseconds{300});

  // Mark, Ctrl-End, Ctrl-P, command.
  session.send(std::string_view("\x00\x1b[1;5F\x10", 8));
  session.readOutput(std::chrono::milliseconds{200});
  session.send("tr x y\r");
  // The cursor ends up after the output; mark the spot and save.
  session.readOutput(std::chrono::milliseconds{1000});
  session.send("Z\x13");
  auto out = stripAnsi(session.readUntil("Saved", std::chrono::milliseconds{10000}));
  BOOST_REQUIRE_MESSAGE(out.find("Saved") != std::string::npos, "editor did not come back");
  std::ranges::replace(body, 'x', 'y');
  BOOST_CHECK(readFile(tmp.path()) == body + "Z");

  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// The input loop read a fixed number of bytes per escape sequence, so the
// tail of a key it did not know was typed into the buffer: F1 inserted
// "OP", F5 "~", Ctrl-PgUp "5~".
BOOST_AUTO_TEST_CASE(e2e_unhandled_keys_insert_nothing) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("hello\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("hello", std::chrono::milliseconds{500});
  // F1, F5, F9, F12, Ctrl-PgUp, Ctrl-PgDn, Ctrl-Insert, Shift-Tab, focus
  // in, the Linux console's F1. Sent one at a time, as a keyboard would.
  for (std::string_view key : {"\x1bOP", "\x1b[15~", "\x1b[20~", "\x1b[24~", "\x1b[5;5~",
                               "\x1b[6;5~", "\x1b[2;5~", "\x1b[Z", "\x1b[I", "\x1b[[A"}) {
    session.send(key);
    session.readOutput(std::chrono::milliseconds{120});
  }
  // The buffer is unmodified, so Esc-c exits without asking.
  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "hello\n");
}

// tmux, screen and the Linux console send Home and End as ESC[1~ / ESC[4~,
// and a terminal in application-cursor mode as ESC O H / ESC O F.
BOOST_AUTO_TEST_CASE(e2e_home_and_end_in_every_encoding) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  struct Case {
    std::string_view home, end;
  };
  for (auto const& c : {Case{"\x1b[H", "\x1b[F"}, Case{"\x1b[1~", "\x1b[4~"},
                        Case{"\x1b[7~", "\x1b[8~"}, Case{"\x1bOH", "\x1bOF"}}) {
    BOOST_TEST_CONTEXT(stripAnsi(c.home)) {
      TempFile tmp("abc\n");
      PtySession session(24, 80, bin, {std::string(tmp.path())});
      session.readUntil("abc", std::chrono::milliseconds{500});
      session.send(c.end);
      session.readOutput(std::chrono::milliseconds{120});
      session.send("Z");
      session.readOutput(std::chrono::milliseconds{120});
      session.send(c.home);
      session.readOutput(std::chrono::milliseconds{120});
      session.send("A\x13");
      session.readUntil("Saved", std::chrono::milliseconds{1000});
      BOOST_CHECK_EQUAL(readFile(tmp.path()), "AabcZ\n");
      session.send("\x1b"
                   "c");
      BOOST_CHECK_EQUAL(session.waitExit(), 0);
    }
  }
}

// Alt-O is "ESC O" with nothing after it: still typed as a letter.
BOOST_AUTO_TEST_CASE(e2e_alt_o_still_inserts_the_letter) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("abc\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("abc", std::chrono::milliseconds{500});
  session.send("\x1bO");
  session.readOutput(std::chrono::milliseconds{300});
  session.send("\x13");
  session.readUntil("Saved", std::chrono::milliseconds{1000});
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "Oabc\n");
  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// Without bracketed paste a terminal paste arrives as keystrokes: every
// Enter auto-indented on top of the pasted indentation (a staircase in
// C/JSON/Python buffers), and every character was its own undo step.
BOOST_AUTO_TEST_CASE(e2e_bracketed_paste_is_one_literal_edit) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("", ".cpp");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  auto start = session.readOutput(std::chrono::milliseconds{300});
  BOOST_CHECK_MESSAGE(start.find("\x1b[?2004h") != std::string::npos,
                      "bracketed paste not enabled");

  // Line breaks arrive as '\r', as a terminal sends them. The tab would
  // run clang-format on the line if it were typed.
  std::string const code = "int main() {\n    if (x) {\n\tfoo();\n    }\n}\n";
  auto typed = code;
  std::ranges::replace(typed, '\n', '\r');
  session.send("\x1b[200~" + typed + "\x1b[201~");
  session.readOutput(std::chrono::milliseconds{300});

  // One history version for the whole paste.
  session.send("\x15");
  auto out = stripAnsi(session.readUntil("v 2/2", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("v 2/2") != std::string::npos, "history badge: " + out);
  session.send("\x15\x13");
  session.readUntil("Saved", std::chrono::milliseconds{1000});
  BOOST_CHECK_EQUAL(readFile(tmp.path()), code);

  session.send("\x1b"
               "c");
  auto end = session.readOutput(std::chrono::milliseconds{300});
  BOOST_CHECK_MESSAGE(end.find("\x1b[?2004l") != std::string::npos, "bracketed paste left on");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// A paste into a prompt inserts its first line; a paste into a read-only
// buffer is dropped (typed, its characters would run as commands there).
BOOST_AUTO_TEST_CASE(e2e_bracketed_paste_in_prompt_and_read_only_buffer) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("one needle here\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("needle", std::chrono::milliseconds{500});

  // Find prompt: only "needle" is taken, and Enter finds it.
  session.send("\x06");
  session.readUntil("Find:", std::chrono::milliseconds{1000});
  session.send("\x1b[200~needle\rsecond line\x1b[201~");
  auto out = stripAnsi(session.readUntil("Find: needle", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Find: needle") != std::string::npos, "prompt: " + out);
  BOOST_CHECK_MESSAGE(out.find("second") == std::string::npos, "prompt took a second line: " + out);
  session.send("\r");
  out = stripAnsi(session.readUntil("1:11", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("1:11") != std::string::npos, "cursor after the match: " + out);

  // Help buffer: 'q' in pasted text must not dismiss it.
  session.send("\x1b?");
  session.readUntil("Press 'q'", std::chrono::milliseconds{1000});
  session.send("\x1b[200~q\x1b[201~");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b[B");
  out = stripAnsi(session.readOutput(std::chrono::milliseconds{300}));
  BOOST_CHECK_MESSAGE(out.find("*help*") != std::string::npos, "help was dismissed: " + out);

  session.send("q");
  session.readOutput(std::chrono::milliseconds{200});
  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "one needle here\n");
}

// A file name ends at its first NUL for the system calls, so a pasted
// "\0" opened the working directory as a text buffer, whose emergency
// copy then went next to that directory, and Save as renamed the buffer
// to the directory.
BOOST_AUTO_TEST_CASE(e2e_file_name_with_a_nul_is_refused) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  PtySession session(24, 80, bin, {});
  session.readOutput(std::chrono::milliseconds{300});
  session.send("hi");
  session.readOutput(std::chrono::milliseconds{200});

  session.send("\x0f");
  session.readUntil("Buffer:", std::chrono::milliseconds{1000});
  session.send(std::string("\x1b[200~") + '\0' + "\x1b[201~\r");
  auto out = stripAnsi(session.readUntil("NUL byte", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("A file name cannot contain a NUL byte") != std::string::npos,
                      "open: " + out);

  session.send("\x13");
  session.readUntil("Save as:", std::chrono::milliseconds{1000});
  session.send(std::string("\x1b[200~x") + '\0' + "y\x1b[201~\r");
  out = stripAnsi(session.readUntil("NUL byte", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("A file name cannot contain a NUL byte") != std::string::npos,
                      "save as: " + out);
  BOOST_CHECK(!std::filesystem::exists("x"));

  // Still the scratch buffer, still modified.
  session.send("\x1b"
               "c");
  out = stripAnsi(session.readUntil("Exiting. Save", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("Exiting. Save temporary buffer?") != std::string::npos &&
                          out.find("*1* [txt] +") != std::string::npos,
                      "exit: " + out);
  session.send("n");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// While one exists, a child that a test crashes on purpose leaves no core
// file behind.
struct NoCoreDumps {
  struct rlimit saved{};
  NoCoreDumps() {
    getrlimit(RLIMIT_CORE, &saved);
    struct rlimit none = saved;
    none.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &none);
  }
  ~NoCoreDumps() { setrlimit(RLIMIT_CORE, &saved); }
};

// A signal that ends the process skips ~SetupTerminal: the shell was left
// in raw mode, on the alternate screen, with mouse reporting on. That
// goes for a signal sent to end the editor and for a crash alike.
BOOST_AUTO_TEST_CASE(e2e_fatal_signal_restores_the_terminal) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  NoCoreDumps noCoreDumps;
  for (int signo : {SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
    BOOST_TEST_CONTEXT("signal " << signo) {
      PtySession session(24, 80, bin);
      session.readOutput(std::chrono::milliseconds{300});
      struct termios raw{};
      BOOST_REQUIRE_EQUAL(tcgetattr(session.masterFd, &raw), 0);
      BOOST_CHECK((raw.c_lflag & ICANON) == 0); // cx is running in raw mode

      kill(session.childPid, signo);
      auto out = session.readOutput(std::chrono::milliseconds{500});
      BOOST_CHECK_MESSAGE(out.find("\x1b[?1049l") != std::string::npos, "alternate screen left on");
      BOOST_CHECK_MESSAGE(out.find("\x1b[?1000l") != std::string::npos, "mouse reporting left on");
      BOOST_CHECK_MESSAGE(out.find("\x1b[?2004l") != std::string::npos, "bracketed paste left on");
      BOOST_CHECK_MESSAGE(out.find("\x1b[?25h") != std::string::npos, "cursor left hidden");

      // The process still dies of the signal, as it would have unhandled.
      int status = 0;
      BOOST_REQUIRE_EQUAL(waitpid(session.childPid, &status, 0), session.childPid);
      session.childPid = -1;
      BOOST_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == signo);

      struct termios after{};
      BOOST_REQUIRE_EQUAL(tcgetattr(session.masterFd, &after), 0);
      BOOST_CHECK((after.c_lflag & ICANON) != 0);
      BOOST_CHECK((after.c_lflag & ECHO) != 0);
    }
  }
}

namespace {

using std::chrono::milliseconds;

bool running(PtySession const& session) {
  int status = 0;
  return waitpid(session.childPid, &status, WNOHANG) == 0;
}

// Type @p text at the start of the buffer shown, then switch to the
// scratch buffer "*s*" and type into that: one modified buffer with a
// file and one without.
void modifyFileAndScratch(PtySession& session, std::string_view text) {
  session.send(text);
  session.readOutput(milliseconds{150});
  session.send("\x0f"); // Ctrl-o
  session.readOutput(milliseconds{150});
  session.send("*s*\r");
  session.readOutput(milliseconds{150});
  session.send("scratch");
  session.readOutput(milliseconds{150});
}

// True if @p out leaves the alternate screen and only then has @p line.
bool afterTeardown(std::string const& out, std::string const& line) {
  auto teardown = out.find("\x1b[?1049l");
  return teardown != std::string::npos && out.find(line, teardown) != std::string::npos;
}

// The files named `<file>.unsaved-…`: what an emergency save leaves next
// to @p file.
std::vector<std::filesystem::path> unsavedCopies(std::filesystem::path const& file) {
  std::vector<std::filesystem::path> copies;
  for (auto const& entry : std::filesystem::directory_iterator(file.parent_path()))
    if (entry.path().string().starts_with(file.string() + ".unsaved-"))
      copies.push_back(entry.path());
  return copies;
}

// An emergency save never writes a file itself: check that @p file still
// reads @p onDisk, that one copy of it holds @p unsaved, and that @p out
// names the copy on the normal screen. Removes the copy.
void checkUnsavedCopy(std::string const& out, std::filesystem::path const& file,
                      std::string const& onDisk, std::string const& unsaved) {
  BOOST_CHECK_EQUAL(readFile(file), onDisk);
  auto copies = unsavedCopies(file);
  BOOST_REQUIRE_EQUAL(copies.size(), 1u);
  BOOST_CHECK_EQUAL(readFile(copies[0]), unsaved);
  BOOST_CHECK_MESSAGE(
      afterTeardown(out, "cx: unsaved " + file.string() + " written to " + copies[0].string()),
      out);
  std::filesystem::remove(copies[0]);
}

} // namespace

// SIGTERM used to end the editor with every unsaved buffer lost. It is
// the exit command now.
BOOST_AUTO_TEST_CASE(e2e_sigterm_exits_when_nothing_is_modified) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("text");
  kill(session.childPid, SIGTERM);
  auto out = session.readOutput(milliseconds{500});
  BOOST_CHECK(out.find("\x1b[?1049l") != std::string::npos);
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

BOOST_AUTO_TEST_CASE(e2e_sigterm_asks_and_a_second_one_writes_copies) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile a("alpha\n"), b("beta\n");
  PtySession session(24, 80, bin, {a.path().string(), b.path().string()});
  session.readUntil("beta");
  session.send("B");
  session.readOutput(milliseconds{150});
  session.send("\x0f"); // Ctrl-o: over to the other file
  session.readOutput(milliseconds{150});
  session.send(a.path().string() + "\r");
  session.readUntil("alpha");
  modifyFileAndScratch(session, "A");

  // The first one asks, as Esc-c does, and saves nothing.
  kill(session.childPid, SIGTERM);
  auto asked = stripAnsi(session.readUntil("Exiting. Save"));
  BOOST_CHECK(asked.find("Exiting. Save") != std::string::npos);
  BOOST_CHECK(running(session));
  BOOST_CHECK_EQUAL(readFile(a.path()), "alpha\n");
  BOOST_CHECK_EQUAL(readFile(b.path()), "beta\n");

  // Nobody answers: the second one writes what has a file to a copy next
  // to it and leaves, with the list of the copies on the normal screen.
  kill(session.childPid, SIGTERM);
  auto out = session.readOutput(milliseconds{500});
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  checkUnsavedCopy(out, a.path(), "alpha\n", "Aalpha\n");
  checkUnsavedCopy(out, b.path(), "beta\n", "Bbeta\n");
  BOOST_CHECK_MESSAGE(afterTeardown(out, "cx: discarded *s*"), out);
}

// Found by the e2e fuzzer: waiting for the rest of an escape sequence
// (or of a paste), the editor retried its read after every signal, so it
// never got back to act on SIGTERM and could only be killed.
BOOST_AUTO_TEST_CASE(e2e_sigterm_ends_the_wait_for_an_unfinished_escape_sequence) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  for (std::string_view unfinished : {"\x1b[1;", "\x1b[200~half a paste"}) {
    BOOST_TEST_CONTEXT("input " << stripAnsi(std::string(unfinished))) {
      TempFile tmp("text\n");
      PtySession session(24, 80, bin, {tmp.path().string()});
      session.readUntil("text");
      session.send("X");
      session.readOutput(milliseconds{150});
      session.send(unfinished); // in one write: the editor reads it as a sequence
      session.readOutput(milliseconds{200});

      kill(session.childPid, SIGTERM);
      auto asked = stripAnsi(session.readUntil("Exiting. Save", milliseconds{1500}));
      BOOST_CHECK(asked.find("Exiting. Save") != std::string::npos);
      kill(session.childPid, SIGTERM);
      auto out = session.readOutput(milliseconds{500});
      BOOST_CHECK_EQUAL(session.waitExit(), 0);
      BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");
      for (auto const& copy : unsavedCopies(tmp.path()))
        std::filesystem::remove(copy);
    }
  }
}

// A SIGTERM used to wait for a command the editor was running (a pipe, a
// formatter, a search) to end on its own.
BOOST_AUTO_TEST_CASE(e2e_sigterm_stops_a_running_command) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("text");
  // Pipe the line through a command that takes a minute: mark, End, Ctrl-P.
  session.send(std::string_view("\x00\x1b[F\x10", 5));
  session.readUntil("Shell command:", milliseconds{1000});
  session.send("sleep 60\r");
  session.readOutput(milliseconds{300});
  BOOST_REQUIRE(running(session));

  auto const t0 = std::chrono::steady_clock::now();
  kill(session.childPid, SIGTERM);
  // Nothing was modified, so the editor exits once the command is stopped.
  BOOST_CHECK_EQUAL(session.waitExit(milliseconds{4000}), 0);
  BOOST_CHECK_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(4));
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");
}

BOOST_AUTO_TEST_CASE(e2e_sigterm_cancels_a_prompt_and_can_be_cancelled_itself) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("text");
  session.send("X");
  session.readOutput(milliseconds{150});
  session.send("\x06"); // Ctrl-f: a Find prompt, where Esc-c does nothing
  session.readOutput(milliseconds{150});

  kill(session.childPid, SIGTERM);
  auto asked = stripAnsi(session.readUntil("Exiting. Save"));
  BOOST_CHECK(asked.find("Exiting. Save") != std::string::npos);

  // Esc cancels the exit. The next SIGTERM is a first one again: it asks.
  session.send("\x1b");
  session.readOutput(milliseconds{200});
  kill(session.childPid, SIGTERM);
  asked = stripAnsi(session.readUntil("Exiting. Save"));
  BOOST_CHECK(asked.find("Exiting. Save") != std::string::npos);
  BOOST_CHECK(running(session));
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");

  session.send("n");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");
}

// An exit the user started is theirs to answer: one SIGTERM does not end
// it over their head, the one after it does.
BOOST_AUTO_TEST_CASE(e2e_sigterm_during_an_exit_started_with_esc_c) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("text");
  session.send("X");
  session.readOutput(milliseconds{150});
  session.send("\x1b"
               "c");
  session.readUntil("Exiting. Save");

  kill(session.childPid, SIGTERM);
  session.readOutput(milliseconds{300});
  BOOST_CHECK(running(session));
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");

  kill(session.childPid, SIGTERM);
  auto out = session.readOutput(milliseconds{500});
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
  checkUnsavedCopy(out, tmp.path(), "text\n", "Xtext\n");
}

BOOST_AUTO_TEST_CASE(e2e_emergency_save_reports_a_file_it_cannot_write) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"f.txt"});
  auto path = dir.path() / "f.txt";
  PtySession session(24, 80, bin, {path.string()});
  session.readUntil("f.txt");
  session.send("X");
  session.readOutput(milliseconds{150});
  std::filesystem::remove_all(dir.path()); // nowhere to save to any more

  kill(session.childPid, SIGTERM);
  session.readUntil("Exiting. Save");
  kill(session.childPid, SIGTERM);
  auto out = session.readOutput(milliseconds{500});
  BOOST_CHECK_MESSAGE(afterTeardown(out, "cx: could not save " + path.string()), out);
  BOOST_CHECK_EQUAL(session.waitExit(), 1);
}

// A bracketed paste whose end marker never came made the editor look
// frozen: every key typed afterwards went into the paste, unseen, and was
// inserted as text (control characters too) once a marker turned up.
BOOST_AUTO_TEST_CASE(e2e_paste_without_end_marker_ends_after_a_pause) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("text");
  session.send("\x1b[200~pasted");
  auto out = stripAnsi(session.readUntil("end marker", milliseconds{4000}));
  BOOST_CHECK_MESSAGE(out.find("Paste ended without its end marker") != std::string::npos, out);
  BOOST_CHECK(out.find("pastedtext") != std::string::npos);

  // Keys are keys again: X is typed, Ctrl-s saves.
  session.send("X\x13");
  session.readUntil("Saved to", milliseconds{1000});
  BOOST_CHECK_EQUAL(readFile(tmp.path()), "pastedXtext\n");
}

// Every input byte used to be followed by a full redraw, so text pasted
// into a terminal without bracketed paste wrote megabytes to it.
BOOST_AUTO_TEST_CASE(e2e_burst_of_input_is_not_redrawn_per_byte) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("end\n");
  PtySession session(24, 80, bin, {tmp.path().string()});
  session.readUntil("end");

  // Fewer bytes than the terminal's input queue holds, in one write.
  std::string const burst(3000, 'x');
  session.send(burst);
  auto out = session.readOutput(milliseconds{500});
  // One redraw per byte is over a megabyte here; a redraw every 30 ms of
  // a burst that takes a few milliseconds is a few kilobytes.
  BOOST_CHECK_MESSAGE(out.size() < 200'000, out.size() << " bytes written");

  // Nothing was dropped, and the last state is what is on the screen.
  BOOST_CHECK(stripAnsi(out).find("1:3001") != std::string::npos);
  session.send("\x13"); // Ctrl-s
  session.readUntil("Saved to");
  BOOST_CHECK_EQUAL(readFile(tmp.path()), burst + "end\n");
}

// Formatting replaces only the lines the formatter changed (it used to
// replace the whole buffer): the result and the cursor must be the same.
BOOST_AUTO_TEST_CASE(e2e_format_rewrites_the_changed_lines) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }
  if (std::system("clang-format --version >/dev/null 2>&1") != 0) {
    BOOST_WARN_MESSAGE(false, "clang-format not found, skipping");
    return;
  }

  std::string good;
  for (int i = 0; i < 300; ++i)
    good += "int v" + std::to_string(i) + ";\n";
  TempFile tmp(good + "int   untidy;\nint  two  ;\n" + good, ".cpp");
  PtySession session(24, 80, bin, {tmp.path().string() + ":301"});
  session.readUntil("untidy");

  session.send("\x1b"
               "f"); // Esc-f: format the buffer
  auto out = stripAnsi(session.readUntil("Formatted", milliseconds{5000}));
  BOOST_CHECK(out.find("Formatted") != std::string::npos);
  BOOST_CHECK(out.find("301:1") != std::string::npos); // the cursor stays on its line

  // A second run has nothing to do.
  session.send("\x1b"
               "f");
  out = stripAnsi(session.readUntil("Already formatted", milliseconds{5000}));
  BOOST_CHECK(out.find("Already formatted") != std::string::npos);

  session.send("\x13"); // Ctrl-s
  session.readUntil("Saved to");
  BOOST_CHECK_EQUAL(readFile(tmp.path()), good + "int untidy;\nint two;\n" + good);
}

// A file changed by another program was only noticed on save. Coming
// back to its buffer now says so, once per change.
BOOST_AUTO_TEST_CASE(e2e_focus_warns_when_the_file_changed_on_disk) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile a("alpha\n"), b("beta\n");
  PtySession session(24, 80, bin, {a.path().string(), b.path().string()});
  session.readUntil("beta");

  // Change a.txt while b is shown, then switch back to a.
  {
    std::ofstream f(a.path(), std::ios::binary);
    f << "rewritten by someone else\n";
  }
  std::filesystem::last_write_time(a.path(), std::filesystem::file_time_type::clock::now() +
                                                 std::chrono::seconds(2));
  auto switchTo = [&](std::filesystem::path const& p) {
    session.send("\x0f"); // Ctrl-o
    session.readOutput(milliseconds{150});
    session.send(p.string() + "\r");
    return stripAnsi(session.readOutput(milliseconds{400}));
  };
  auto const warning = "changed on disk since it was loaded or saved";
  auto out = switchTo(a.path());
  BOOST_CHECK_MESSAGE(out.find(warning) != std::string::npos, out);

  // Away and back again: already reported. A status message stays up for
  // a while, so put another one there first: save the unchanged b.
  out = switchTo(b.path());
  session.send("\x13"); // Ctrl-s
  session.readUntil("Saved to");
  out = switchTo(a.path());
  BOOST_CHECK_MESSAGE(out.find(warning) == std::string::npos, out);
  BOOST_CHECK(out.find("Saved to") != std::string::npos);
  BOOST_CHECK_EQUAL(readFile(b.path()), "beta\n");
}

// Closing a terminal normally sends SIGHUP, but only to a process the
// terminal controls. Without it the editor just saw the end of its input
// and exited, with every unsaved buffer lost.
BOOST_AUTO_TEST_CASE(e2e_closed_input_without_sighup_writes_copies) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("text\n");
  TempDir home; // the error log goes here: the terminal cannot be restored

  // cx on a pty that is not its controlling terminal: closing the master
  // ends its input (read() returns 0) and no signal is sent.
  struct winsize ws{};
  ws.ws_row = 24;
  ws.ws_col = 80;
  int masterFd = -1, slaveFd = -1;
  BOOST_REQUIRE_EQUAL(openpty(&masterFd, &slaveFd, nullptr, nullptr, &ws), 0);
  pid_t pid = fork();
  BOOST_REQUIRE(pid >= 0);
  if (pid == 0) {
    setsid(); // a session of its own, and the open slave does not become its terminal
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
      dup2(slaveFd, fd);
    close(slaveFd);
    close(masterFd);
    setenv("HOME", home.path().c_str(), 1);
    std::string binCopy = bin, arg = tmp.path().string();
    char* argv[] = {binCopy.data(), arg.data(), nullptr};
    execv(bin.c_str(), argv);
    _exit(127);
  }
  close(slaveFd);

  // Wait for the file to be shown, type into it, give that time to arrive.
  std::string shown;
  char buf[4096];
  for (int i = 0; i < 200 && shown.find("text") == std::string::npos; ++i) {
    struct pollfd pfd{masterFd, POLLIN, 0};
    if (poll(&pfd, 1, 10) > 0)
      if (auto n = ::read(masterFd, buf, sizeof(buf)); n > 0)
        shown.append(buf, static_cast<std::size_t>(n));
  }
  BOOST_REQUIRE(shown.find("text") != std::string::npos);
  BOOST_REQUIRE_EQUAL(::write(masterFd, "X", 1), 1);
  usleep(200000);
  close(masterFd);

  int status = 0;
  bool exited = false;
  for (int i = 0; i < 300 && !exited; ++i) {
    exited = waitpid(pid, &status, WNOHANG) == pid;
    if (!exited)
      usleep(10000);
  }
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  BOOST_REQUIRE(exited);
  BOOST_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  BOOST_CHECK_EQUAL(readFile(tmp.path()), "text\n");
  auto copies = unsavedCopies(tmp.path());
  BOOST_REQUIRE_EQUAL(copies.size(), 1u);
  BOOST_CHECK_EQUAL(readFile(copies[0]), "Xtext\n");
  std::filesystem::remove(copies[0]);
}

// A crash, or a signal sent to end the editor at once, took every unsaved
// buffer with it. The handler now writes the ones that have a file to
// copies next to them and lists those, once the terminal is back.
BOOST_AUTO_TEST_CASE(e2e_fatal_signal_writes_the_modified_files_to_copies) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  NoCoreDumps noCoreDumps;
  for (int signo : {SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
    BOOST_TEST_CONTEXT("signal " << signo) {
      TempFile tmp("text\n");
      PtySession session(24, 80, bin, {tmp.path().string()});
      session.readUntil("text");
      modifyFileAndScratch(session, "X");

      kill(session.childPid, signo);
      auto out = session.readOutput(milliseconds{500});
      BOOST_CHECK_MESSAGE(afterTeardown(out, "cx: discarded *s*"), out);
      // The buffer may be damaged, so the file stays as it was last saved.
      checkUnsavedCopy(out, tmp.path(), "text\n", "Xtext\n");

      // It is still the signal that ends the process.
      int status = 0;
      BOOST_REQUIRE_EQUAL(waitpid(session.childPid, &status, 0), session.childPid);
      session.childPid = -1;
      BOOST_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == signo);
    }
  }
}

// Replacing a match and piping a region used to delete and then insert:
// two history versions, the first one with the text gone and nothing in
// its place. Each is one undo step now.
BOOST_AUTO_TEST_CASE(e2e_replace_and_pipe_are_one_history_version_each) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempFile tmp("foo bar foo\n");
  PtySession session(24, 80, bin, {std::string(tmp.path())});
  session.readUntil("foo", std::chrono::milliseconds{500});

  // Esc-% foo -> LONGER, replace the first match, quit the walk.
  session.send("\x1b%");
  session.readUntil("Replace:", std::chrono::milliseconds{1000});
  session.send("foo\r");
  session.readUntil("with:", std::chrono::milliseconds{1000});
  session.send("LONGER\r");
  session.readUntil("Replace this?", std::chrono::milliseconds{1000});
  session.send("y");
  session.readUntil("Replace this?", std::chrono::milliseconds{1000});
  session.send("q");
  session.readOutput(std::chrono::milliseconds{200});

  // Pipe the whole first line through tr: mark at line start, End, Ctrl-P.
  session.send(std::string_view("\x1b[H\x00\x1b[F\x10", 8));
  session.readUntil("Shell command:", std::chrono::milliseconds{1000});
  session.send("tr a-z A-Z\r");
  session.readUntil("LONGER BAR FOO", std::chrono::milliseconds{3000});

  // Load, replace, pipe: three versions; one step back undoes the pipe.
  session.send("\x15");
  auto out = stripAnsi(session.readUntil("v 3/3", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("v 3/3") != std::string::npos, "history badge: " + out);
  session.send("[");
  out = stripAnsi(session.readUntil("LONGER bar foo", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("LONGER bar foo") != std::string::npos, "previous version: " + out);

  session.send("\x1b\x1b");
  session.readUntil("cancelled", std::chrono::milliseconds{1000});
  session.send("\x1b"
               "cn");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}

// The directory browser kept the listing from when the directory was
// first opened, so a file created later never appeared in it.
BOOST_AUTO_TEST_CASE(e2e_directory_browser_shows_files_created_since) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    BOOST_WARN_MESSAGE(false, "cx binary not found, skipping E2E test");
    return;
  }

  TempDir dir({"first.txt"});
  PtySession session(24, 100, bin, {(dir.path() / "first.txt").string()});
  session.readUntil("first.txt", std::chrono::milliseconds{500});

  // Directory browser, then back to the file with Enter on it.
  session.send("\x1b"
               "d");
  session.readUntil("drwx", std::chrono::milliseconds{1000});
  session.send("\x1b[B\x1b[B\r");
  session.readOutput(std::chrono::milliseconds{200});

  std::ofstream(dir.path() / "second.txt") << "new";
  session.send("\x1b"
               "d");
  auto out = stripAnsi(session.readUntil("second.txt", std::chrono::milliseconds{1000}));
  BOOST_CHECK_MESSAGE(out.find("second.txt") != std::string::npos, "listing: " + out);

  session.send("\x1b"
               "c");
  BOOST_CHECK_EQUAL(session.waitExit(), 0);
}
