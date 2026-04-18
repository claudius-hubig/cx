#define BOOST_TEST_MODULE TextBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "tests/test_helpers.hpp"

#include <fstream>
#include <string>

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
  buf.revert();
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

  buf.revert();
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

  buf.revert();
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
  buf.revert();
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
  buf.revert();
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
