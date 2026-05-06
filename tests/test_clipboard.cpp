#define BOOST_TEST_MODULE Clipboard
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "clipboard.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

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
