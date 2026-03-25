#define BOOST_TEST_MODULE TextBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "tests/test_helpers.hpp"

#include <fstream>
#include <string>

// --- Constructor ---

BOOST_AUTO_TEST_CASE(name_only_no_path) {
  TextBuffer buf("scratch");
  BOOST_CHECK_EQUAL(buf.name(), "scratch");
  BOOST_CHECK(!buf.hasPath());
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(from_file) {
  TempFile tmp("hello world");
  TextBuffer buf(tmp.path().string(), tmp.path());
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
  TextBuffer buf(missing.string(), missing);
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
  TextBuffer buf("scratch");
  buf.insert('x');
  BOOST_CHECK(!buf.save());
  BOOST_CHECK(buf.modified()); // still modified
}

BOOST_AUTO_TEST_CASE(save_writes_and_clears_modified) {
  TempFile tmp;
  TextBuffer buf(tmp.path().string(), tmp.path());
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
  TextBuffer buf(tmp.path().string(), tmp.path());
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
  TextBuffer buf("scratch");
  buf.insert('x');
  auto result = buf.revert();
  BOOST_CHECK(!result);
}

BOOST_AUTO_TEST_CASE(revert_reloads_file_contents) {
  TempFile tmp("hello world");
  TextBuffer buf(tmp.path().string(), tmp.path());
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

BOOST_AUTO_TEST_CASE(revert_resets_point_to_start) {
  TempFile tmp("abc\ndef\nghi");
  TextBuffer buf(tmp.path().string(), tmp.path());
  buf.moveEnd();
  buf.moveDown(ColIdx{80});
  // Point should be past the start.
  BOOST_CHECK(buf.point() > (BufferPoint{0, 0}));

  buf.revert();
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(revert_clears_mark) {
  TempFile tmp("some text");
  TextBuffer buf(tmp.path().string(), tmp.path());
  buf.toggleMark();
  BOOST_CHECK(buf.mark().has_value());

  buf.revert();
  BOOST_CHECK(!buf.mark().has_value());
}

BOOST_AUTO_TEST_CASE(revert_clears_modified) {
  TempFile tmp("data");
  TextBuffer buf(tmp.path().string(), tmp.path());
  buf.insert('X');
  BOOST_CHECK(buf.modified());

  buf.revert();
  BOOST_CHECK(!buf.modified());
}

BOOST_AUTO_TEST_CASE(revert_picks_up_external_changes) {
  TempFile tmp("original");
  TextBuffer buf(tmp.path().string(), tmp.path());

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
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u); // 3 lines + trailing empty

  // Modify and revert.
  buf.insert('X');
  buf.revert();
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u);
  BOOST_CHECK(!buf.modified());
}

// --- SetPath ---

BOOST_AUTO_TEST_CASE(setPath_updates_name_and_hasPath) {
  TextBuffer buf("scratch");
  BOOST_CHECK(!buf.hasPath());
  std::filesystem::path p = "/tmp/cx_test_setpath";
  buf.setPath(p);
  BOOST_CHECK(buf.hasPath());
  BOOST_CHECK_EQUAL(buf.name(), p.string());
}

// --- Mode detection ---

BOOST_AUTO_TEST_CASE(scratch_buffer_defaults_to_text_mode) {
  TextBuffer buf("scratch");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(cpp_extension_sets_cpp_mode) {
  TempFile tmp("int main() {}", ".cpp");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(c_extension_sets_cpp_mode) {
  TempFile tmp("int main() {}", ".c");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(h_extension_sets_cpp_mode) {
  TempFile tmp("", ".h");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(hpp_extension_sets_cpp_mode) {
  TempFile tmp("", ".hpp");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(ixx_extension_sets_cpp_mode) {
  TempFile tmp("", ".ixx");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(cxx_extension_sets_cpp_mode) {
  TempFile tmp("", ".cxx");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(txt_extension_stays_text_mode) {
  TempFile tmp("hello", ".txt");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(no_extension_stays_text_mode) {
  TempFile tmp("hello");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(missing_cpp_file_still_detects_mode) {
  std::filesystem::path p = "/tmp/cx_test_nonexistent.cpp";
  TextBuffer buf(p.string(), p);
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(setMode_overrides_detected_mode) {
  TempFile tmp("int x;", ".cpp");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
  buf.setMode(BufferMode::Txt);
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}

BOOST_AUTO_TEST_CASE(setPath_redetects_mode) {
  TextBuffer buf("scratch");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
  buf.setPath("/tmp/cx_test_foo.cpp");
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
}

BOOST_AUTO_TEST_CASE(setPath_to_txt_resets_mode) {
  TempFile tmp("", ".cpp");
  TextBuffer buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
  buf.setPath("/tmp/cx_test_foo.txt");
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
}
