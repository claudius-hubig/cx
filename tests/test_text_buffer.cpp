#define BOOST_TEST_MODULE TextBuffer
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "tests/test_helpers.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

// --- Constructor ---

BOOST_AUTO_TEST_CASE(name_only_no_path) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK_EQUAL(buf.name(), "scratch");
  BOOST_CHECK(!buf.hasPath());
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(from_file) {
  TempFile tmp("hello world");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.hasPath());
  BOOST_CHECK(!buf.modified());
  // Data should match file contents — check via lineCol after moveEnd.
  buf.moveEnd();
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 1u);
  BOOST_CHECK_EQUAL(col, 12u); // "hello world" = 11 chars, 1-indexed
}

BOOST_AUTO_TEST_CASE(from_missing_file) {
  std::filesystem::path missing = "/tmp/cx_test_nonexistent_file_12345";
  TB<TextBuffer> buf(missing.string(), missing);
  BOOST_CHECK(buf.hasPath());
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
  // Buffer should be empty
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 1u);
  BOOST_CHECK_EQUAL(col, 1u);
}

// --- Save ---

BOOST_AUTO_TEST_CASE(save_no_path_returns_false) {
  TB<TextBuffer> buf("scratch");
  buf.insert('x');
  BOOST_CHECK(!buf.save());
  BOOST_CHECK(buf.modified()); // still modified
}

BOOST_AUTO_TEST_CASE(save_writes_and_clears_modified) {
  TempFile tmp;
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  buf.insert('A');
  buf.insert('B');
  BOOST_CHECK(buf.modified());
  BOOST_CHECK(buf.save());
  BOOST_CHECK(!buf.modified());

  // Verify file content
  std::ifstream f(tmp.path(), std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  BOOST_CHECK_EQUAL(content, "AB");
}

BOOST_AUTO_TEST_CASE(save_truncates_existing) {
  TempFile tmp("old content that is longer");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  // Delete all characters by backspacing from the end.
  buf.moveEnd();
  while (buf.point() > (BufferPoint{0, 0}))
    buf.deleteBeforePoint();
  buf.insert('X');
  BOOST_CHECK(buf.save());

  std::ifstream f(tmp.path(), std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  BOOST_CHECK_EQUAL(content, "X");
}

// --- changedOnDisk ---

BOOST_AUTO_TEST_CASE(changedOnDisk_false_for_pathless) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK(!buf.changedOnDisk());
}

BOOST_AUTO_TEST_CASE(changedOnDisk_false_immediately_after_load) {
  TempFile tmp("hello");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(!buf.changedOnDisk());
}

BOOST_AUTO_TEST_CASE(changedOnDisk_detects_external_modification) {
  TempFile tmp("hello");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  // Bump mtime far enough that filesystem-level resolution can't miss it.
  auto newTime = std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2);
  std::error_code ec;
  std::filesystem::last_write_time(tmp.path(), newTime, ec);
  BOOST_REQUIRE(!ec);
  // Also change size so we don't depend purely on mtime resolution.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
    f << "different content";
  }
  BOOST_CHECK(buf.changedOnDisk());
}

BOOST_AUTO_TEST_CASE(changedOnDisk_resets_after_save) {
  TempFile tmp("hello");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  // External change.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
    f << "outside";
  }
  // Saving rewrites the file and updates the captured stamp.
  BOOST_CHECK(buf.save());
  BOOST_CHECK(!buf.changedOnDisk());
}

// --- Revert ---

BOOST_AUTO_TEST_CASE(revert_no_path_returns_error) {
  TB<TextBuffer> buf("scratch");
  buf.insert('x');
  auto result = buf.revert();
  BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(revert_reloads_file_contents) {
  TempFile tmp("hello world");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  // Modify the buffer.
  buf.moveEnd();
  buf.insert('!');
  BOOST_CHECK(buf.modified());

  // Revert should reload original content.
  auto result = buf.revert();
  BOOST_CHECK(result.has_value());
  BOOST_CHECK(!buf.modified());

  // Check that the buffer content matches the file.
  buf.moveEnd();
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 1u);
  BOOST_CHECK_EQUAL(col, 12u); // "hello world" = 11 chars, 1-indexed
}

// Reverting under a history browse let the browsing view commit an old
// version afterwards: the history then ended in a text the buffer did not
// hold, and the next single-line edit was recorded as a delta against it
// (out-of-bounds line patches when stepping back through it).
BOOST_AUTO_TEST_CASE(revert_refused_while_browsing_history) {
  TempFile f("one\ntwo\nthree\nfour\n");
  TB<TextBuffer> buf(f.path().string(), f.path());
  buf.setPoint(1, 0);
  buf.view.deleteLine();
  buf.view.deleteLine();
  buf.view.deleteLine(); // "one", ""
  BOOST_REQUIRE(buf.view.enterHistoryMode());
  buf.view.stepHistory(-1);
  auto shown = buf.lineCount();
  auto result = buf.revert();
  BOOST_CHECK(!result.has_value());
  BOOST_CHECK_EQUAL(buf.lineCount(), shown);
  buf.view.exitHistoryModeCommit();
  // The history's newest version is the text the buffer holds.
  auto const& newest = buf.history().at(buf.history().size() - 1);
  BOOST_REQUIRE_EQUAL(newest.lines.size(), buf.lineCount());
  for (std::size_t i = 0; i < buf.lineCount(); ++i)
    BOOST_CHECK_EQUAL(*newest.lines[i], buf.lineContent(i));
  BOOST_CHECK(buf.revert().has_value());
}

BOOST_AUTO_TEST_CASE(revert_bumps_docversion_and_clamps_view) {
  // revert() only reloads content; it does not touch view state. The
  // BufferView detects the docVersion change on next access and clamps
  // point/mark to valid positions in the reloaded buffer. Callers (e.g.
  // Screen) are responsible for any explicit cursor reset.
  TempFile tmp("abc\ndef\nghi");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  buf.moveEnd();
  buf.moveDown(ColIdx{80});
  BOOST_CHECK(buf.point() > (BufferPoint{0, 0}));

  auto v0 = buf.docVersion();
  BOOST_CHECK(buf.revert());
  BOOST_CHECK(buf.docVersion() != v0);
  // Point remains valid after revert (clamped if needed).
  BOOST_CHECK(buf.point().line < buf.lineCount());
  BOOST_CHECK(buf.point().offset <= buf.lineContent(buf.point().line).size());
}

BOOST_AUTO_TEST_CASE(revert_clears_modified) {
  TempFile tmp("data");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  buf.insert('X');
  BOOST_CHECK(buf.modified());

  BOOST_CHECK(buf.revert());
  BOOST_CHECK(!buf.modified());
}

BOOST_AUTO_TEST_CASE(revert_picks_up_external_changes) {
  TempFile tmp("original");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());

  // Externally write new content to the file.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
    f << "changed content";
  }

  BOOST_CHECK(buf.revert());
  // Buffer should now contain "changed content".
  buf.moveEnd();
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 1u);
  BOOST_CHECK_EQUAL(col, 16u); // "changed content" = 15 chars, 1-indexed
}

BOOST_AUTO_TEST_CASE(revert_multiline) {
  TempFile tmp("aaa\nbbb\nccc\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u); // 3 lines + trailing empty

  // Modify and revert.
  buf.insert('X');
  BOOST_CHECK(buf.revert());
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u);
  BOOST_CHECK(!buf.modified());
}

// --- SetPath ---

BOOST_AUTO_TEST_CASE(setPath_updates_name_and_hasPath) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK(!buf.hasPath());
  std::filesystem::path p = "/tmp/cx_test_setpath";
  buf.setPath(p);
  BOOST_CHECK(buf.hasPath());
  BOOST_CHECK_EQUAL(buf.name(), p.string());
}

// --- Mode detection ---

BOOST_AUTO_TEST_CASE(scratch_buffer_defaults_to_text_mode) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(cpp_extension_sets_cpp_mode) {
  TempFile tmp("int main() {}", ".cpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(c_extension_sets_cpp_mode) {
  TempFile tmp("int main() {}", ".c");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(h_extension_sets_cpp_mode) {
  TempFile tmp("", ".h");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(hpp_extension_sets_cpp_mode) {
  TempFile tmp("", ".hpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(ixx_extension_sets_cpp_mode) {
  TempFile tmp("", ".ixx");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(cxx_extension_sets_cpp_mode) {
  TempFile tmp("", ".cxx");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(txt_extension_stays_text_mode) {
  TempFile tmp("hello", ".txt");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(no_extension_stays_text_mode) {
  TempFile tmp("hello");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(missing_cpp_file_still_detects_mode) {
  std::filesystem::path p = "/tmp/cx_test_nonexistent.cpp";
  TB<TextBuffer> buf(p.string(), p);
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(setMode_overrides_detected_mode) {
  TempFile tmp("int x;", ".cpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
  buf.setMode(BufferMode::Txt);
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(setPath_redetects_mode) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
  buf.setPath("/tmp/cx_test_foo.cpp");
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(setPath_to_txt_resets_mode) {
  TempFile tmp("", ".cpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
  buf.setPath("/tmp/cx_test_foo.txt");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(sh_extension_sets_sh_mode) {
  TempFile tmp("echo hi\n", ".sh");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Sh);
}

BOOST_AUTO_TEST_CASE(bash_extension_sets_sh_mode) {
  TempFile tmp("echo hi\n", ".bash");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Sh);
}

BOOST_AUTO_TEST_CASE(shebang_bash_sets_sh_mode_without_extension) {
  TempFile tmp("#!/bin/bash\necho hi\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Sh);
}

BOOST_AUTO_TEST_CASE(shebang_usr_bin_env_sh_sets_sh_mode) {
  TempFile tmp("#!/usr/bin/env sh\necho hi\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Sh);
}

BOOST_AUTO_TEST_CASE(non_shell_shebang_stays_text_mode) {
  TempFile tmp("#!/usr/bin/perl\nprint 1;\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(yml_extension_sets_yml_mode) {
  TempFile tmp("a: 1\n", ".yml");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Yml);
}

BOOST_AUTO_TEST_CASE(yaml_extension_sets_yml_mode) {
  TempFile tmp("a: 1\n", ".yaml");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Yml);
}

BOOST_AUTO_TEST_CASE(toml_extension_sets_tml_mode) {
  TempFile tmp("a = 1\n", ".toml");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Tml);
}

BOOST_AUTO_TEST_CASE(py_extension_sets_py_mode) {
  TempFile tmp("x = 1\n", ".py");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Py);
}

BOOST_AUTO_TEST_CASE(pyi_extension_sets_py_mode) {
  TempFile tmp("def f() -> int: ...\n", ".pyi");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Py);
}

BOOST_AUTO_TEST_CASE(shebang_python_sets_py_mode_without_extension) {
  TempFile tmp("#!/usr/bin/env python3\nprint(1)\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Py);
}

// --- Line ending detection ---

BOOST_AUTO_TEST_CASE(line_endings_unix_default_for_scratch_buffer) {
  TB<TextBuffer> buf("scratch");
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
}

BOOST_AUTO_TEST_CASE(line_endings_unix_for_empty_file) {
  TempFile tmp("");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
}

BOOST_AUTO_TEST_CASE(line_endings_unix_for_lf_only_file) {
  TempFile tmp("alpha\nbeta\ngamma\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u); // 3 lines + trailing empty
}

BOOST_AUTO_TEST_CASE(line_endings_crlf_detected) {
  TempFile tmp("alpha\r\nbeta\r\ngamma\r\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Crlf);
  // Lines retain their trailing '\r' until normalized.
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha\r");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "beta\r");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "gamma\r");
}

BOOST_AUTO_TEST_CASE(line_endings_cr_only_detected) {
  TempFile tmp("alpha\rbeta\rgamma\r");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Cr);
  // Without '\n' splits, the whole file collapses into a single line
  // containing embedded '\r' bytes.
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha\rbeta\rgamma\r");
}

BOOST_AUTO_TEST_CASE(line_endings_mixed_crlf_and_lf) {
  TempFile tmp("alpha\r\nbeta\ngamma\r\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Mixed);
}

BOOST_AUTO_TEST_CASE(line_endings_mixed_cr_and_lf) {
  TempFile tmp("alpha\rbeta\ngamma");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Mixed);
}

BOOST_AUTO_TEST_CASE(line_endings_acknowledge_clears_style) {
  TempFile tmp("hi\r\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Crlf);
  buf.acknowledgeLineEndingStyle();
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
}

// --- Line ending normalization ---

BOOST_AUTO_TEST_CASE(normalize_crlf_strips_trailing_cr) {
  TempFile tmp("alpha\r\nbeta\r\ngamma\r\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  auto v0 = buf.docVersion();
  buf.normalizeLineEndings();
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
  BOOST_CHECK(buf.modified());
  BOOST_CHECK(buf.docVersion() != v0);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "beta");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "gamma");
}

BOOST_AUTO_TEST_CASE(normalize_cr_splits_lines) {
  TempFile tmp("alpha\rbeta\rgamma");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  buf.normalizeLineEndings();
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
  BOOST_CHECK_EQUAL(buf.lineCount(), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "beta");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "gamma");
}

BOOST_AUTO_TEST_CASE(normalize_cr_with_trailing_cr) {
  // A trailing '\r' should produce a trailing empty line, matching the
  // LF case where "a\nb\n" yields three lines (a, b, empty).
  TempFile tmp("alpha\rbeta\r");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  buf.normalizeLineEndings();
  BOOST_CHECK_EQUAL(buf.lineCount(), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "beta");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "");
}

BOOST_AUTO_TEST_CASE(normalize_unix_is_noop) {
  TempFile tmp("alpha\nbeta\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  auto v0 = buf.docVersion();
  buf.normalizeLineEndings();
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK_EQUAL(buf.docVersion(), v0);
}

BOOST_AUTO_TEST_CASE(normalize_mixed_is_noop) {
  // Mixed endings cannot be unambiguously translated and must be left
  // alone; the user is expected to see the status-line warning.
  TempFile tmp("alpha\r\nbeta\ngamma\r\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Mixed);
  auto v0 = buf.docVersion();
  buf.normalizeLineEndings();
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Mixed);
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK_EQUAL(buf.docVersion(), v0);
}

BOOST_AUTO_TEST_CASE(revert_redetects_line_endings) {
  TempFile tmp("hi\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
  // Externally overwrite the file with CRLF content and revert.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
    f << "hi\r\n";
  }
  BOOST_CHECK(buf.revert());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Crlf);
}

BOOST_AUTO_TEST_CASE(normalize_line_endings_records_version) {
  TempFile tmp("a\r\nb\r\n");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Crlf);
  auto before = buf.history().size();
  buf.normalizeLineEndings();
  BOOST_CHECK_EQUAL(buf.history().size(), before + 1);
  BOOST_CHECK(buf.lineEndingStyle() == LineEndingStyle::Unix);
}

// recordVersion previously appended unconditionally. While browsingHistory()
// is true a debug helper that bypasses BufferView's readonly() gate could
// corrupt the history chain by inserting a new version mid-browse. The
// override now silently no-ops in that state.
BOOST_AUTO_TEST_CASE(recordVersion_noop_while_browsing_history) {
  TempFile tmp("hi\n");
  TextBuffer buf(tmp.path().string(), tmp.path());
  buf.recordVersion({0, 0});
  auto before = buf.history().size();
  buf.setBrowsingHistory(true);
  buf.recordVersion({0, 0});
  BOOST_CHECK_EQUAL(buf.history().size(), before);
  buf.setBrowsingHistory(false);
  buf.recordVersion({0, 0});
  BOOST_CHECK_EQUAL(buf.history().size(), before + 1);
}

// Reverting to an externally-emptied file used to leave _hasConflicts set
// from before the revert. The reset now happens at the top of
// loadFromFile so all early-return paths share it.
BOOST_AUTO_TEST_CASE(loadFromFile_resets_hasConflicts_on_empty_revert) {
  TempFile tmp("<<<<<<< HEAD\nfoo\n=======\nbar\n>>>>>>> feat\n");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_REQUIRE(buf.hasConflicts());
  // Externally truncate the file and revert.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
  }
  BOOST_CHECK(buf.revert());
  BOOST_CHECK(!buf.hasConflicts());
}
