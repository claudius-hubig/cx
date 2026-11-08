#define BOOST_TEST_MODULE Clipboard
#include <boost/test/unit_test.hpp>

#include "clipboard.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace fs = std::filesystem;

namespace {

// RAII helper: create an empty executable file at `path`.
struct Executable {
  fs::path path;
  Executable(fs::path p) : path(std::move(p)) {
    std::ofstream(path) << "#!/bin/sh\n";
    ::chmod(path.c_str(), 0755);
  }
  ~Executable() {
    std::error_code ec;
    fs::remove(path, ec);
  }
};

// RAII helper: create a directory and remove it on scope exit.
struct TempDir {
  fs::path path;
  TempDir() {
    path = fs::temp_directory_path() /
           fs::path("cx-clipboard-test-" + std::to_string(::getpid()) + "-" +
                    std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(find_on_path_returns_empty_for_empty_inputs) {
  using clipboard_detail::findOnPath;
  BOOST_CHECK(findOnPath("", "xclip").empty());
  BOOST_CHECK(findOnPath("/usr/bin", "").empty());
  BOOST_CHECK(findOnPath("", "").empty());
}

BOOST_AUTO_TEST_CASE(find_on_path_returns_empty_when_missing) {
  using clipboard_detail::findOnPath;
  TempDir dir;
  BOOST_CHECK(findOnPath(dir.path.string(), "definitely-not-installed-xyz").empty());
}

BOOST_AUTO_TEST_CASE(find_on_path_finds_executable_in_first_dir) {
  using clipboard_detail::findOnPath;
  TempDir dir;
  Executable exe(dir.path / "fake_xclip");
  auto found = findOnPath(dir.path.string(), "fake_xclip");
  BOOST_CHECK_EQUAL(found, exe.path.string());
}

BOOST_AUTO_TEST_CASE(find_on_path_finds_executable_in_later_dir) {
  using clipboard_detail::findOnPath;
  TempDir a, b, c;
  Executable exe(c.path / "fake_xclip");
  std::string path = a.path.string() + ":" + b.path.string() + ":" + c.path.string();
  auto found = findOnPath(path, "fake_xclip");
  BOOST_CHECK_EQUAL(found, exe.path.string());
}

BOOST_AUTO_TEST_CASE(find_on_path_returns_first_hit) {
  using clipboard_detail::findOnPath;
  TempDir a, b;
  Executable first(a.path / "fake_xclip");
  Executable second(b.path / "fake_xclip");
  std::string path = a.path.string() + ":" + b.path.string();
  auto found = findOnPath(path, "fake_xclip");
  BOOST_CHECK_EQUAL(found, first.path.string());
}

BOOST_AUTO_TEST_CASE(find_on_path_skips_non_executable) {
  using clipboard_detail::findOnPath;
  TempDir dir;
  // Plain non-exec file with the right name shouldn't match.
  std::ofstream(dir.path / "fake_xclip") << "not executable";
  ::chmod((dir.path / "fake_xclip").c_str(), 0644);
  BOOST_CHECK(findOnPath(dir.path.string(), "fake_xclip").empty());
}

BOOST_AUTO_TEST_CASE(find_on_path_handles_empty_path_components) {
  using clipboard_detail::findOnPath;
  TempDir dir;
  Executable exe(dir.path / "fake_xclip");
  // Adjacent colons produce empty components which must be skipped.
  std::string path = "::" + dir.path.string() + "::";
  auto found = findOnPath(path, "fake_xclip");
  BOOST_CHECK_EQUAL(found, exe.path.string());
}

BOOST_AUTO_TEST_CASE(find_on_path_handles_trailing_slash) {
  using clipboard_detail::findOnPath;
  TempDir dir;
  Executable exe(dir.path / "fake_xclip");
  std::string path = dir.path.string() + "/";
  auto found = findOnPath(path, "fake_xclip");
  BOOST_CHECK_EQUAL(found, exe.path.string());
}

BOOST_AUTO_TEST_CASE(clipboard_inactive_when_display_unset) {
  ::unsetenv("DISPLAY");
  Clipboard c;
  BOOST_CHECK(!c.active());
  // Writes and reads should silently no-op.
  c.setPrimary("hello");
  c.setClipboard("world");
  BOOST_CHECK(!c.getClipboard().has_value());
}

BOOST_AUTO_TEST_CASE(clipboard_inactive_when_display_empty) {
  ::setenv("DISPLAY", "", 1);
  Clipboard c;
  BOOST_CHECK(!c.active());
  c.setPrimary("hello");
  BOOST_CHECK(!c.getClipboard().has_value());
}

BOOST_AUTO_TEST_CASE(clipboard_inactive_when_no_backend_on_path) {
  ::setenv("DISPLAY", ":0", 1);
  // Use a synthetic PATH that contains no xclip / xsel. This relies on
  // the directory being empty of those names. We use an empty TempDir
  // and override PATH; we don't permanently mutate the environment.
  TempDir dir;
  std::string oldPath = std::getenv("PATH") ? std::getenv("PATH") : "";
  ::setenv("PATH", dir.path.c_str(), 1);
  Clipboard c;
  BOOST_CHECK(!c.active());
  // Restore PATH so other tests don't see an empty PATH.
  if (!oldPath.empty())
    ::setenv("PATH", oldPath.c_str(), 1);
  else
    ::unsetenv("PATH");
}

// ---------------------------------------------------------------------------
// End-to-end tests using the fake xclip / xsel scripts in
// tests/fake_clipboard/. CMake passes the directory path through
// CX_FAKE_CLIPBOARD_BIN_DIR. Each fixture sets up a private bin dir
// and a private storage dir, points DISPLAY/PATH/CX_FAKE_CLIPBOARD_DIR
// at them, and restores the environment on destruction.

namespace {

// Saves and restores a single env var across its lifetime.
struct ScopedEnv {
  std::string name;
  std::string oldValue;
  bool hadValue;

  ScopedEnv(char const* n, char const* v) : name(n) {
    char const* cur = std::getenv(n);
    hadValue = cur != nullptr;
    if (hadValue)
      oldValue = cur;
    if (v)
      ::setenv(n, v, 1);
    else
      ::unsetenv(n);
  }
  ~ScopedEnv() {
    if (hadValue)
      ::setenv(name.c_str(), oldValue.c_str(), 1);
    else
      ::unsetenv(name.c_str());
  }
};

// Test fixture: a temp bin dir into which the requested fake scripts
// are symlinked, plus a temp storage dir for the selections, plus
// scoped overrides of DISPLAY, PATH and CX_FAKE_CLIPBOARD_DIR.
struct FakeClipboardEnv {
  TempDir bin;
  TempDir storage;
  ScopedEnv display{"DISPLAY", ":0"};
  ScopedEnv storageDir{"CX_FAKE_CLIPBOARD_DIR", nullptr};
  ScopedEnv path{"PATH", nullptr};

  FakeClipboardEnv(bool installXclip, bool installXsel) {
    fs::path src = CX_FAKE_CLIPBOARD_BIN_DIR;
    if (installXclip)
      fs::create_symlink(src / "xclip", bin.path / "xclip");
    if (installXsel)
      fs::create_symlink(src / "xsel", bin.path / "xsel");

    ::setenv("CX_FAKE_CLIPBOARD_DIR", storage.path.c_str(), 1);
    // Keep /usr/bin and /bin on PATH so the shell scripts can find cat,
    // mv, mkdir, etc.
    std::string fullPath = bin.path.string() + ":/usr/bin:/bin";
    ::setenv("PATH", fullPath.c_str(), 1);
  }

  // Poll until `path` exists or `timeout` elapses.
  static bool waitForFile(fs::path const& target,
                          std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (fs::exists(target))
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fs::exists(target);
  }

  // Read entire file contents.
  static std::string slurp(fs::path const& target) {
    std::ifstream in(target, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(e2e_set_clipboard_writes_via_xclip) {
  FakeClipboardEnv env(/*xclip=*/true, /*xsel=*/false);
  Clipboard c;
  BOOST_REQUIRE(c.active());

  c.setClipboard("hello world");
  auto target = env.storage.path / "clipboard";
  BOOST_REQUIRE(env.waitForFile(target));
  BOOST_CHECK_EQUAL(env.slurp(target), "hello world");
}

BOOST_AUTO_TEST_CASE(e2e_set_primary_writes_via_xclip) {
  FakeClipboardEnv env(true, false);
  Clipboard c;
  BOOST_REQUIRE(c.active());

  c.setPrimary("selected text");
  auto target = env.storage.path / "primary";
  BOOST_REQUIRE(env.waitForFile(target));
  BOOST_CHECK_EQUAL(env.slurp(target), "selected text");
}

BOOST_AUTO_TEST_CASE(e2e_get_clipboard_reads_via_xclip) {
  FakeClipboardEnv env(true, false);
  // Pre-populate the selection file.
  std::ofstream(env.storage.path / "clipboard") << "from another app";
  Clipboard c;
  BOOST_REQUIRE(c.active());

  auto out = c.getClipboard();
  BOOST_REQUIRE(out.has_value());
  BOOST_CHECK_EQUAL(*out, "from another app");
}

BOOST_AUTO_TEST_CASE(e2e_get_clipboard_returns_nullopt_when_empty) {
  FakeClipboardEnv env(true, false);
  Clipboard c;
  // No file present, fake xclip outputs nothing → readSync sees empty
  // string → getClipboard returns nullopt.
  BOOST_CHECK(!c.getClipboard().has_value());
}

BOOST_AUTO_TEST_CASE(e2e_get_primary_reads_via_xclip) {
  FakeClipboardEnv env(true, false);
  // Pre-populate the PRIMARY selection (what a middle-click would paste).
  std::ofstream(env.storage.path / "primary") << "mouse selection";
  Clipboard c;
  BOOST_REQUIRE(c.active());

  auto out = c.getPrimary();
  BOOST_REQUIRE(out.has_value());
  BOOST_CHECK_EQUAL(*out, "mouse selection");
}

BOOST_AUTO_TEST_CASE(e2e_get_primary_returns_nullopt_when_empty) {
  FakeClipboardEnv env(true, false);
  Clipboard c;
  BOOST_CHECK(!c.getPrimary().has_value());
}

BOOST_AUTO_TEST_CASE(get_primary_nullopt_when_inactive) {
  ::unsetenv("DISPLAY");
  Clipboard c;
  BOOST_CHECK(!c.active());
  BOOST_CHECK(!c.getPrimary().has_value());
}

BOOST_AUTO_TEST_CASE(e2e_set_primary_dedups_consecutive_identical) {
  FakeClipboardEnv env(true, false);
  Clipboard c;
  BOOST_REQUIRE(c.active());

  c.setPrimary("same");
  BOOST_REQUIRE(env.waitForFile(env.storage.path / "primary"));
  // Wait a moment to make sure any second invocation would have logged.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  c.setPrimary("same"); // should be a no-op
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Different content on the third call — should produce a fresh
  // invocation.
  c.setPrimary("different");
  // Poll until the file content changes.
  auto target = env.storage.path / "primary";
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (env.slurp(target) == "different")
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  BOOST_CHECK_EQUAL(env.slurp(target), "different");

  // Only two xclip invocations should have occurred (the duplicate
  // was skipped before fork). Each invocation appends one line.
  auto log = env.slurp(env.storage.path / "calls.log");
  std::size_t lineCount = 0;
  for (char ch : log)
    if (ch == '\n')
      ++lineCount;
  BOOST_CHECK_EQUAL(lineCount, 2u);
}

BOOST_AUTO_TEST_CASE(e2e_falls_back_to_xsel_when_xclip_missing) {
  FakeClipboardEnv env(/*xclip=*/false, /*xsel=*/true);
  Clipboard c;
  BOOST_REQUIRE(c.active());

  c.setClipboard("via xsel");
  auto target = env.storage.path / "clipboard";
  BOOST_REQUIRE(env.waitForFile(target));
  BOOST_CHECK_EQUAL(env.slurp(target), "via xsel");

  // Sanity-check that it really was xsel that ran.
  auto log = env.slurp(env.storage.path / "calls.log");
  BOOST_CHECK(log.find("xsel ") != std::string::npos);
  BOOST_CHECK(log.find("xclip ") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(e2e_setclipboard_does_not_dedup) {
  // Cut/copy is rare and we explicitly do not dedup — verify.
  FakeClipboardEnv env(true, false);
  Clipboard c;
  BOOST_REQUIRE(c.active());

  c.setClipboard("repeat");
  BOOST_REQUIRE(env.waitForFile(env.storage.path / "clipboard"));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  c.setClipboard("repeat");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  auto log = env.slurp(env.storage.path / "calls.log");
  std::size_t lineCount = 0;
  for (char ch : log)
    if (ch == '\n')
      ++lineCount;
  BOOST_CHECK_EQUAL(lineCount, 2u);
}
