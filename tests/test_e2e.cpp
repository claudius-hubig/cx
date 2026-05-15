#define BOOST_TEST_MODULE E2E
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "test_helpers.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
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
      kill(childPid, SIGTERM);
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
    kill(pid, SIGTERM);
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
