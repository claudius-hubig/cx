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
