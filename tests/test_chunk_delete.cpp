#define BOOST_TEST_MODULE ChunkDelete
#include <boost/test/unit_test.hpp>

#include "buffer/help_buffer.hpp"
#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// --- deleteChunkAtPoint ---

BOOST_AUTO_TEST_CASE(chunkDelete_word) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.deleteChunkAtPoint();
  // Should delete "hello" (non-blank chunk).
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(buf.point().offset, 0u);
  // Remaining text should start with " world".
  buf.moveEnd();
  auto [line2, col2] = buf.lineCol();
  BOOST_CHECK_EQUAL(col2, 7u); // " world" = 6 chars + 1 for 1-indexed = 7
}

BOOST_AUTO_TEST_CASE(chunkDelete_spaces) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello   world");
  buf.moveHome();
  // Move past "hello".
  for (int i = 0; i < 5; ++i)
    buf.moveRight();
  buf.deleteChunkAtPoint();
  // Should delete "   " (blank chunk).
  // After deletion, point is at 'w'.
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(buf.point().offset, 5u);
  // Total remaining: "helloworld" = 10 chars.
  buf.moveEnd();
  auto [line2, col2] = buf.lineCol();
  BOOST_CHECK_EQUAL(col2, 11u); // 10 chars + 1-indexed
}

BOOST_AUTO_TEST_CASE(chunkDelete_crosses_newline) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc\ndef");
  buf.gotoLine(1, RowIdx{10}); // go to start of line 0
  buf.moveEnd();               // end of "abc" (offset 3)
  buf.deleteChunkAtPoint();
  // At end of line, the implicit newline is "blank". Deleting blank chunk
  // should delete the newline (joining lines), then stop at 'd' (non-blank).
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 1u);
}

BOOST_AUTO_TEST_CASE(chunkDelete_at_end_noop) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc");
  auto before = buf.point();
  buf.deleteChunkAtPoint();
  BOOST_CHECK(buf.point() == before);
}

BOOST_AUTO_TEST_CASE(chunkDelete_readonly_noop) {
  TB<HelpBuffer> buf;
  auto before = buf.point();
  buf.deleteChunkAtPoint();
  BOOST_CHECK(buf.point() == before);
  BOOST_CHECK(!buf.modified());
}

// --- deleteChunkBeforePoint ---

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_word) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  // Point at end (offset 11).
  buf.deleteChunkBeforePoint();
  // Should delete "world" backward (non-blank chunk).
  auto [line, col] = buf.lineCol();
  // Remaining: "hello " = 6 chars, point at offset 6.
  BOOST_CHECK_EQUAL(buf.point().offset, 6u);
}

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_spaces) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello   world");
  buf.moveHome();
  // Move to 'w' (offset 8).
  for (int i = 0; i < 8; ++i)
    buf.moveRight();
  buf.deleteChunkBeforePoint();
  // Should delete "   " backward (blank chunk).
  BOOST_CHECK_EQUAL(buf.point().offset, 5u);
}

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_crosses_newline) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc\ndef");
  buf.gotoLine(2, RowIdx{10}); // at start of "def"
  buf.deleteChunkBeforePoint();
  // At start of line, the newline before is "blank". Deleting blank chunk backward
  // should delete the newline (joining lines).
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
}

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_at_start_noop) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc");
  buf.moveHome();
  auto before = buf.point();
  buf.deleteChunkBeforePoint();
  BOOST_CHECK(buf.point() == before);
}

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_utf8) {
  TB<TextBuffer> buf("test");
  // Insert "abc" then two CJK chars.
  insertString(buf, "abc");
  buf.insert(0x4E2Du); // 中
  buf.insert(0x6587u); // 文
  buf.deleteChunkBeforePoint();
  // Should delete non-blank chunk backward: "中文" and "abc" are all non-blank.
  // Actually the CJK chars and ASCII are all non-sep, so it deletes "abc中文".
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

// --- One undo step per chunk ---

// A chunk used to be deleted one character at a time, each recorded as
// its own version: a long word flooded the bounded history and evicted
// everything older.
BOOST_AUTO_TEST_CASE(chunkDelete_records_one_version) {
  TB<TextBuffer> buf("test");
  insertString(buf, std::string(500, 'x') + " tail");
  buf.moveHome();
  auto versions = buf.history().size();
  buf.deleteChunkAtPoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), " tail");
  BOOST_CHECK_EQUAL(buf.history().size(), versions + 1);
}

BOOST_AUTO_TEST_CASE(chunkDeleteBefore_records_one_version) {
  TB<TextBuffer> buf("test");
  insertString(buf, "head " + std::string(500, 'x'));
  auto versions = buf.history().size();
  buf.deleteChunkBeforePoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "head ");
  BOOST_CHECK(buf.point() == (BufferPoint{0, 5}));
  BOOST_CHECK_EQUAL(buf.history().size(), versions + 1);
}

BOOST_AUTO_TEST_CASE(chunkDelete_blank_run_across_lines_is_one_version) {
  TB<TextBuffer> buf("test");
  insertString(buf, "a  \n\n  b");
  buf.setPoint(0, 1);
  auto versions = buf.history().size();
  buf.deleteChunkAtPoint();
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ab");
  BOOST_CHECK_EQUAL(buf.history().size(), versions + 1);
}

BOOST_AUTO_TEST_CASE(chunkDelete_keeps_mark_on_its_text) {
  TB<TextBuffer> buf("test");
  insertString(buf, "one two\nthree four");
  buf.setPoint(1, 6); // before "four"
  buf.toggleMark();
  buf.setPoint(0, 4);       // before "two"
  buf.deleteChunkAtPoint(); // "two"
  BOOST_REQUIRE(buf.mark().has_value());
  BOOST_CHECK(*buf.mark() == (BufferPoint{1, 6}));
  buf.deleteChunkAtPoint(); // the newline joins the lines
  BOOST_CHECK_EQUAL(buf.lineContent(0), "one three four");
  BOOST_CHECK(*buf.mark() == (BufferPoint{0, 10}));
  // A mark inside the deleted chunk collapses onto the point.
  buf.setPoint(0, 4);
  buf.toggleMark();
  buf.toggleMark();
  buf.setPoint(0, 6);
  buf.toggleMark(); // mark inside "three"
  buf.setPoint(0, 4);
  buf.deleteChunkAtPoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "one  four");
  BOOST_CHECK(*buf.mark() == (BufferPoint{0, 4}));
}
