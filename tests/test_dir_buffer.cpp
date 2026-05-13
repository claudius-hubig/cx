#define BOOST_TEST_MODULE DirBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/dir_buffer.hpp"
#include "test_helpers.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// RAII helper to create and clean up a temporary directory tree.
struct TmpDir {
  fs::path path;

  TmpDir() {
    path = fs::temp_directory_path() / ("cx_test_dir_" + std::to_string(getpid()));
    fs::create_directories(path);
  }

  ~TmpDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }

  void createFile(std::string const& name, std::string const& content = "") {
    std::ofstream ofs(path / name);
    ofs << content;
  }

  void createDir(std::string const& name) { fs::create_directories(path / name); }
};

BOOST_AUTO_TEST_CASE(is_readonly) {
  TmpDir tmp;
  TB<DirBuffer> buf(tmp.path);
  BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(dot_and_dotdot_at_top) {
  TmpDir tmp;
  tmp.createFile("hello.txt");
  tmp.createDir("subdir");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  BOOST_REQUIRE_GE(entries.size(), 2u);
  BOOST_CHECK_EQUAL(entries[0].name, ".");
  BOOST_CHECK_EQUAL(entries[1].name, "..");
  BOOST_CHECK(entries[0].isDir);
  BOOST_CHECK(entries[1].isDir);
}

BOOST_AUTO_TEST_CASE(directories_before_files) {
  TmpDir tmp;
  tmp.createFile("aaa.txt");
  tmp.createDir("zzz_dir");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  // After . and .., directories come before files.
  // zzz_dir should appear before aaa.txt despite alphabetical order.
  std::size_t dirIdx = 0;
  std::size_t fileIdx = 0;
  for (std::size_t i = 2; i < entries.size(); ++i) {
    if (entries[i].name == "zzz_dir")
      dirIdx = i;
    if (entries[i].name == "aaa.txt")
      fileIdx = i;
  }
  BOOST_CHECK_LT(dirIdx, fileIdx);
}

BOOST_AUTO_TEST_CASE(navigate_changes_directory) {
  TmpDir tmp;
  tmp.createDir("child");

  TB<DirBuffer> buf(tmp.path);
  auto origDir = buf.directory();
  buf.navigate(tmp.path / "child");
  BOOST_CHECK_EQUAL(buf.directory(), fs::weakly_canonical(fs::absolute(tmp.path / "child")));
  BOOST_CHECK_NE(buf.directory(), origDir);
}

BOOST_AUTO_TEST_CASE(hidden_files_filtered_by_default) {
  TmpDir tmp;
  tmp.createFile(".hidden");
  tmp.createFile("visible.txt");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  bool foundHidden = false;
  bool foundVisible = false;
  for (auto& e : entries) {
    if (e.name == ".hidden")
      foundHidden = true;
    if (e.name == "visible.txt")
      foundVisible = true;
  }
  BOOST_CHECK(!foundHidden);
  BOOST_CHECK(foundVisible);
}

BOOST_AUTO_TEST_CASE(toggle_hidden_shows_dotfiles) {
  TmpDir tmp;
  tmp.createFile(".hidden");
  tmp.createFile("visible.txt");

  TB<DirBuffer> buf(tmp.path);
  BOOST_CHECK(!buf.showingHidden());
  buf.toggleHidden(buf.point());
  BOOST_CHECK(buf.showingHidden());

  auto const& entries = buf.entries();
  bool foundHidden = false;
  for (auto& e : entries) {
    if (e.name == ".hidden")
      foundHidden = true;
  }
  BOOST_CHECK(foundHidden);
}

BOOST_AUTO_TEST_CASE(selectedPath_and_selectedIsDir) {
  TmpDir tmp;
  tmp.createFile("file.txt");
  tmp.createDir("subdir");

  TB<DirBuffer> buf(tmp.path);
  // First entry is '.', which is a directory.
  BOOST_CHECK(buf.selectedIsDir(buf.point()));
  BOOST_CHECK_EQUAL(buf.selectedPath(buf.point()), buf.directory());
}

BOOST_AUTO_TEST_CASE(cannot_mark_dot_or_dotdot) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  // Cursor on line 0 (.)
  buf.toggleDeleteMark(buf.point());
  BOOST_CHECK(buf.markedPaths().empty());

  // Move to line 1 (..)
  buf.movePhysicalDown();
  buf.toggleDeleteMark(buf.point());
  BOOST_CHECK(buf.markedPaths().empty());
}

BOOST_AUTO_TEST_CASE(mark_and_unmark) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  // Find file.txt index.
  std::size_t fileIdx = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].name == "file.txt") {
      fileIdx = i;
      break;
    }
  }
  BOOST_REQUIRE_GT(fileIdx, 1u);

  // Move cursor to file.txt.
  for (std::size_t i = 0; i < fileIdx; ++i)
    buf.movePhysicalDown();

  buf.toggleDeleteMark(buf.point());
  BOOST_CHECK(buf.isMarked(fileIdx));
  BOOST_CHECK_EQUAL(buf.markedPaths().size(), 1u);

  // Unmark.
  buf.toggleDeleteMark(buf.point());
  BOOST_CHECK(!buf.isMarked(fileIdx));
  BOOST_CHECK(buf.markedPaths().empty());
}

BOOST_AUTO_TEST_CASE(execute_delete_removes_files) {
  TmpDir tmp;
  tmp.createFile("to_delete.txt", "data");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  std::size_t fileIdx = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].name == "to_delete.txt") {
      fileIdx = i;
      break;
    }
  }

  for (std::size_t i = 0; i < fileIdx; ++i)
    buf.movePhysicalDown();

  buf.toggleDeleteMark(buf.point());
  BOOST_CHECK(fs::exists(tmp.path / "to_delete.txt"));

  auto result = buf.executeDelete();
  BOOST_CHECK_EQUAL(result.count, 1u);
  BOOST_CHECK(result.error.empty());
  BOOST_CHECK(!fs::exists(tmp.path / "to_delete.txt"));
}

BOOST_AUTO_TEST_CASE(execute_delete_removes_directories) {
  TmpDir tmp;
  tmp.createDir("subdir");
  tmp.createFile("subdir/inner.txt", "data");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  std::size_t dirIdx = 0;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].name == "subdir") {
      dirIdx = i;
      break;
    }
  }

  for (std::size_t i = 0; i < dirIdx; ++i)
    buf.movePhysicalDown();

  buf.toggleDeleteMark(buf.point());
  auto result = buf.executeDelete();
  BOOST_CHECK_EQUAL(result.count, 1u);
  BOOST_CHECK(result.error.empty());
  BOOST_CHECK(!fs::exists(tmp.path / "subdir"));
}

BOOST_AUTO_TEST_CASE(entry_has_owner_and_group) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  for (auto& e : entries) {
    // Owner and group should be non-empty (populated from stat).
    BOOST_CHECK(!e.owner.empty());
    BOOST_CHECK(!e.group.empty());
  }
}

BOOST_AUTO_TEST_CASE(entry_has_permissions) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  auto const& entries = buf.entries();
  for (auto& e : entries) {
    // Permissions should be 10 chars like "drwxr-xr-x" or "-rw-r--r--".
    BOOST_CHECK_EQUAL(e.permissions.size(), 10u);
    char first = e.permissions[0];
    BOOST_CHECK(first == 'd' || first == '-' || first == 'l');
  }
}

BOOST_AUTO_TEST_CASE(render_produces_output) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{24}, ColIdx{80}, renderParams(buf));
  BOOST_CHECK(!out.empty());
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
}

BOOST_AUTO_TEST_CASE(display_lines_have_del_prefix_area) {
  TmpDir tmp;
  tmp.createFile("file.txt");

  TB<DirBuffer> buf(tmp.path);
  // Render without marks — lines should start with 4 spaces.
  std::string out;
  buf.render(out, RowIdx{24}, ColIdx{80}, renderParams(buf));
  // The display lines in the rendered output should contain "    " prefix.
  // We can check via the entries — each display line begins with 4-space prefix.
  // Verify by finding "file.txt" in the rendered output without "DEL" prefix.
  BOOST_CHECK(out.find("DEL ") == std::string::npos);

  // Now mark a file and re-render — should contain "DEL ".
  auto const& entries = buf.entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].name == "file.txt") {
      for (std::size_t j = 0; j < i; ++j)
        buf.movePhysicalDown();
      buf.toggleDeleteMark(buf.point());
      break;
    }
  }

  out.clear();
  buf.render(out, RowIdx{24}, ColIdx{80}, renderParams(buf));
  BOOST_CHECK(out.find("DEL ") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(navigate_clears_marks) {
  TmpDir tmp;
  tmp.createFile("file.txt");
  tmp.createDir("sub");

  TB<DirBuffer> buf(tmp.path);
  // Mark file.txt.
  auto const& entries = buf.entries();
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].name == "file.txt") {
      for (std::size_t j = 0; j < i; ++j)
        buf.movePhysicalDown();
      buf.toggleDeleteMark(buf.point());
      break;
    }
  }
  BOOST_CHECK(!buf.markedPaths().empty());

  buf.navigate(tmp.path / "sub");
  BOOST_CHECK(buf.markedPaths().empty());
}

// --- rename + mkdir ---

BOOST_AUTO_TEST_CASE(rename_renames_file_on_disk) {
  TmpDir tmp;
  tmp.createFile("old.txt", "data");
  TB<DirBuffer> buf(tmp.path);
  // Find the entry for old.txt.
  std::size_t idx = 0;
  for (std::size_t i = 2; i < buf.entries().size(); ++i) {
    if (buf.entries()[i].name == "old.txt") {
      idx = i;
      break;
    }
  }
  BOOST_REQUIRE_GT(idx, 0u);
  auto r = buf.renameEntry({idx, 0}, "new.txt");
  BOOST_CHECK(r.ok);
  BOOST_CHECK(fs::exists(tmp.path / "new.txt"));
  BOOST_CHECK(!fs::exists(tmp.path / "old.txt"));
  // Listing reflects the new name and cursor is on it.
  bool found = false;
  for (auto const& e : buf.entries())
    if (e.name == "new.txt") {
      found = true;
      break;
    }
  BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(rename_refuses_dot) {
  TmpDir tmp;
  TB<DirBuffer> buf(tmp.path);
  auto r = buf.renameEntry({0, 0}, "x");
  BOOST_CHECK(!r.ok);
}

BOOST_AUTO_TEST_CASE(rename_rejects_collision) {
  TmpDir tmp;
  tmp.createFile("a.txt");
  tmp.createFile("b.txt");
  TB<DirBuffer> buf(tmp.path);
  std::size_t idx = 0;
  for (std::size_t i = 2; i < buf.entries().size(); ++i)
    if (buf.entries()[i].name == "a.txt")
      idx = i;
  BOOST_REQUIRE_GT(idx, 0u);
  auto r = buf.renameEntry({idx, 0}, "b.txt");
  BOOST_CHECK(!r.ok);
  BOOST_CHECK(fs::exists(tmp.path / "a.txt"));
}

BOOST_AUTO_TEST_CASE(rename_rejects_slash_in_name) {
  TmpDir tmp;
  tmp.createFile("a.txt");
  TB<DirBuffer> buf(tmp.path);
  std::size_t idx = 0;
  for (std::size_t i = 2; i < buf.entries().size(); ++i)
    if (buf.entries()[i].name == "a.txt")
      idx = i;
  BOOST_REQUIRE_GT(idx, 0u);
  auto r = buf.renameEntry({idx, 0}, "sub/a.txt");
  BOOST_CHECK(!r.ok);
}

BOOST_AUTO_TEST_CASE(mkdir_creates_subdir) {
  TmpDir tmp;
  TB<DirBuffer> buf(tmp.path);
  auto r = buf.makeSubdir("newdir");
  BOOST_CHECK(r.ok);
  BOOST_CHECK(fs::is_directory(tmp.path / "newdir"));
  bool found = false;
  for (auto const& e : buf.entries())
    if (e.name == "newdir") {
      found = true;
      BOOST_CHECK(e.isDir);
    }
  BOOST_CHECK(found);
}

BOOST_AUTO_TEST_CASE(mkdir_rejects_existing) {
  TmpDir tmp;
  tmp.createDir("existing");
  TB<DirBuffer> buf(tmp.path);
  auto r = buf.makeSubdir("existing");
  BOOST_CHECK(!r.ok);
}

BOOST_AUTO_TEST_CASE(mkdir_rejects_slash_in_name) {
  TmpDir tmp;
  TB<DirBuffer> buf(tmp.path);
  auto r = buf.makeSubdir("a/b");
  BOOST_CHECK(!r.ok);
}

// --- sanitizeNssField ---
//
// Owner / group strings come from NSS lookups (getpwuid, getgrgid),
// which on LDAP/NIS-backed systems can deliver bytes a user did not
// author. The dir listing renders directly to the terminal without
// going through Buffer::render's escape path, so any control byte
// inside the owner / group columns would be written verbatim.

BOOST_AUTO_TEST_CASE(sanitizeNssField_passes_ascii_through) {
  using dir_buffer_detail::sanitizeNssField;
  BOOST_CHECK_EQUAL(sanitizeNssField(""), "");
  BOOST_CHECK_EQUAL(sanitizeNssField("root"), "root");
  BOOST_CHECK_EQUAL(sanitizeNssField("user_42"), "user_42");
  // Printable specials that turn up in real usernames.
  BOOST_CHECK_EQUAL(sanitizeNssField("svc-deploy$"), "svc-deploy$");
}

BOOST_AUTO_TEST_CASE(sanitizeNssField_replaces_esc) {
  using dir_buffer_detail::sanitizeNssField;
  // ESC ('\x1b') would open a CSI sequence if written raw.
  BOOST_CHECK_EQUAL(sanitizeNssField(std::string("a\x1b[31m", 6)), "a?[31m");
  // BEL ('\x07') is the OSC terminator.
  BOOST_CHECK_EQUAL(sanitizeNssField(std::string("u\x07v", 3)), "u?v");
}

BOOST_AUTO_TEST_CASE(sanitizeNssField_replaces_low_controls_and_del) {
  using dir_buffer_detail::sanitizeNssField;
  BOOST_CHECK_EQUAL(sanitizeNssField(std::string("\x00\x09\x1f\x7f", 4)), "????");
}

BOOST_AUTO_TEST_CASE(sanitizeNssField_replaces_c1_range) {
  using dir_buffer_detail::sanitizeNssField;
  // 8-bit C1 controls (0x80..0x9F) can act as one-byte CSI / OSC
  // introducers on terminals running in 8-bit mode. Replace them.
  BOOST_CHECK_EQUAL(sanitizeNssField(std::string("\x80\x9f", 2)), "??");
}

BOOST_AUTO_TEST_CASE(sanitizeNssField_preserves_utf8_above_c1) {
  using dir_buffer_detail::sanitizeNssField;
  // Bytes >= 0xA0 are passed through. Real-world UTF-8 lead/continuation
  // bytes fall in 0xC2..0xFD / 0xA0..0xBF respectively. The function is
  // length-preserving on these.
  std::string in = "user\xC2\xA0name"; // U+00A0 NO-BREAK SPACE
  BOOST_CHECK_EQUAL(sanitizeNssField(in), in);
}

BOOST_AUTO_TEST_CASE(sanitizeNssField_preserves_byte_length) {
  using dir_buffer_detail::sanitizeNssField;
  // Critical for the dir listing's column alignment math, which uses
  // size() (not visualWidth) when padding owner/group columns.
  std::string in(std::string("\x01\x02\x1b a\x7f", 6));
  auto out = sanitizeNssField(in);
  BOOST_CHECK_EQUAL(out.size(), in.size());
  BOOST_CHECK_EQUAL(out, "??? a?");
}
