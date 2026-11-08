#define BOOST_TEST_MODULE Movement
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// --- moveWordRight ---

BOOST_AUTO_TEST_CASE(wordRight_from_start) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.moveWordRight();
  // Lands one past the last char of "hello" (offset 5, on the space)
  // so typing immediately extends the word.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 5}));
}

BOOST_AUTO_TEST_CASE(wordRight_mid_word) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.moveRight(); // 'h' -> 'e'
  buf.moveWordRight();
  // Step forward 1 cp from offset 1 -> offset 2 ('l').
  // 'l' is non-sep so the skip-seps loop is a no-op.
  // Advance past the word: l(2) → l(3) → o(4) → space(5), stop at 5.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 5}));
}

BOOST_AUTO_TEST_CASE(wordRight_skips_spaces) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello   world");
  buf.moveHome();
  buf.moveWordRight();
  // Lands one past "hello" = on the first space at offset 5.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 5}));
  buf.moveWordRight();
  // Steps forward to offset 6 (space). Skips spaces 6,7. Arrives at
  // 'w'(8). Advances past "world" to offset 13 (end of line).
  BOOST_CHECK(buf.point() == (BufferPoint{0, 13}));
}

BOOST_AUTO_TEST_CASE(wordRight_crosses_lines) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc\ndef");
  buf.gotoLine(1, RowIdx{10}); // go to line 0
  buf.moveWordRight();
  // Past "abc" on line 0 = offset 3 (end of line).
  BOOST_CHECK(buf.point() == (BufferPoint{0, 3}));
  buf.moveWordRight();
  // From {0,3}: no step (offset == size); cross to line 1 offset 0.
  // Skip seps: 'd' is non-sep. Advance past "def" = offset 3.
  BOOST_CHECK(buf.point() == (BufferPoint{1, 3}));
}

BOOST_AUTO_TEST_CASE(wordRight_at_end) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello");
  // Point is at end.
  auto before = buf.point();
  buf.moveWordRight();
  BOOST_CHECK(buf.point() == before); // no-op
}

BOOST_AUTO_TEST_CASE(wordRight_then_typing_extends_word) {
  // Regression: pressing Ctrl-Right and typing 'e' should produce
  // "abcde", not "abced". Verifies the cursor lands past the word.
  TB<TextBuffer> buf("test");
  insertString(buf, "abcd");
  buf.moveHome();
  buf.moveWordRight();
  buf.insert('e');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "abcde");
}

// --- moveWordLeft ---

BOOST_AUTO_TEST_CASE(wordLeft_from_end) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  // Point at end (offset 11).
  buf.moveWordLeft();
  // Steps back from offset 11 to 'd'(10). Skip seps: none (on non-sep 'd').
  // Walk backward to start of "world" = 'w' at offset 6.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 6}));
}

BOOST_AUTO_TEST_CASE(wordLeft_mid_word) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  // Move to 'r' (offset 8).
  for (int i = 0; i < 8; ++i)
    buf.moveRight();
  buf.moveWordLeft();
  // Steps back from 'r'(8) to 'o'(7). Skip seps: none. Walk backward to start of word.
  // 'o' is in "world" starting at 'w'(6). So lands at 'w'(6).
  BOOST_CHECK(buf.point() == (BufferPoint{0, 6}));
}

BOOST_AUTO_TEST_CASE(wordLeft_skips_spaces) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello   world");
  buf.moveHome();
  // Move to 'w' (offset 8).
  for (int i = 0; i < 8; ++i)
    buf.moveRight();
  buf.moveWordLeft();
  // Steps back from 'w'(8) to ' '(7). Skip seps backward: 7,6,5 to 'o'(4).
  // Walk backward to start of "hello" = 'h'(0).
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(wordLeft_crosses_lines) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc\ndef");
  // Point at end of "def" (line 1, offset 3).
  buf.gotoLine(2, RowIdx{10});
  buf.moveWordLeft();
  // Steps back from offset 0 of line 1 -> end of line 0 -> 'c'(2).
  // Walk backward to 'a'(0).
  // Actually: gotoLine(2) puts point at start of line 1 (offset 0).
  // moveWordLeft: offset=0, line>0 -> cross to prev line end offset=3, step back to 'c'(2).
  // On non-sep 'c'. Walk backward to start of "abc" = 'a'(0).
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(wordLeft_at_start) {
  TB<TextBuffer> buf("test");
  insertString(buf, "hello");
  buf.moveHome();
  auto before = buf.point();
  buf.moveWordLeft();
  BOOST_CHECK(buf.point() == before); // no-op
}

// --- moveParagraphUp ---

BOOST_AUTO_TEST_CASE(paragraphUp_from_middle) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\n\nccc\nddd");
  buf.gotoLine(5, RowIdx{10}); // on "ddd"
  buf.moveParagraphUp();
  // Should land at start of "ccc" paragraph (line 3, 0-based).
  BOOST_CHECK_EQUAL(buf.point().line, 3u);
  BOOST_CHECK_EQUAL(buf.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(paragraphUp_from_first_line) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(1, RowIdx{10});
  buf.moveParagraphUp();
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(paragraphUp_skips_blank_lines) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\n\n\nbbb");
  buf.gotoLine(4, RowIdx{10}); // on "bbb"
  buf.moveParagraphUp();
  // Should land at start of "aaa" (line 0).
  BOOST_CHECK_EQUAL(buf.point().line, 0u);
}

// --- moveParagraphDown ---

BOOST_AUTO_TEST_CASE(paragraphDown_from_middle) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\n\nccc\nddd");
  buf.gotoLine(1, RowIdx{10}); // on "aaa"
  buf.moveParagraphDown();
  // Should skip "aaa","bbb" (non-blank), then blank line, land on "ccc" (line 3).
  BOOST_CHECK_EQUAL(buf.point().line, 3u);
}

BOOST_AUTO_TEST_CASE(paragraphDown_from_last_line) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb");
  buf.gotoLine(2, RowIdx{10}); // on "bbb"
  buf.moveParagraphDown();
  // At end, should stay on last line.
  BOOST_CHECK_EQUAL(buf.point().line, 1u);
}

BOOST_AUTO_TEST_CASE(paragraphDown_skips_blank_lines) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\n\n\nbbb");
  buf.gotoLine(1, RowIdx{10}); // on "aaa"
  buf.moveParagraphDown();
  // Skip non-blank ("aaa"), then blank lines, land on "bbb" (line 3).
  BOOST_CHECK_EQUAL(buf.point().line, 3u);
}

// --- movePhysicalUp/Down ---

BOOST_AUTO_TEST_CASE(physicalUp_basic) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(2, RowIdx{10}); // on "bbb"
  buf.movePhysicalUp();
  BOOST_CHECK_EQUAL(buf.point().line, 0u);
}

BOOST_AUTO_TEST_CASE(physicalUp_preserves_desired_col) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abcde\nfg\nhijkl");
  buf.gotoLine(1, RowIdx{10});
  buf.moveEnd();          // end of "abcde", offset 5
  buf.movePhysicalDown(); // to "fg", clamps to offset 2
  buf.movePhysicalDown(); // to "hijkl", restores desired col 5
  auto [line, col] = buf.lineCol();
  BOOST_CHECK_EQUAL(line, 3u);
  BOOST_CHECK_EQUAL(col, 6u); // 1-indexed, col 5 visual = col 6 displayed
}

BOOST_AUTO_TEST_CASE(physicalUp_boundary) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc");
  buf.movePhysicalUp();
  // Already on first line — goes to offset 0.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(physicalDown_basic) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(1, RowIdx{10}); // on "aaa"
  buf.movePhysicalDown();
  BOOST_CHECK_EQUAL(buf.point().line, 1u);
}

BOOST_AUTO_TEST_CASE(physicalDown_boundary) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abc");
  buf.moveHome();
  buf.movePhysicalDown();
  // Already on last line — goes to end.
  BOOST_CHECK(buf.point() == (BufferPoint{0, 3}));
}

// --- pageUp/Down ---

BOOST_AUTO_TEST_CASE(pageUp_basic) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i) {
    insertString(buf, "line");
    if (i < 19)
      buf.insert('\n');
  }
  buf.gotoLine(15, RowIdx{10});
  buf.pageUp(RowIdx{10}, ColIdx{20}, true);
  // Should have moved up by half screen (5 lines).
  BOOST_CHECK(buf.point().line < 14u);
}

BOOST_AUTO_TEST_CASE(pageUp_clamps_at_start) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(2, RowIdx{10});
  buf.pageUp(RowIdx{10}, ColIdx{20}, true);
  BOOST_CHECK_EQUAL(buf.point().line, 0u);
}

BOOST_AUTO_TEST_CASE(pageDown_basic) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i) {
    insertString(buf, "line");
    if (i < 19)
      buf.insert('\n');
  }
  buf.gotoLine(5, RowIdx{10});
  buf.pageDown(RowIdx{10}, ColIdx{20}, true);
  BOOST_CHECK(buf.point().line > 4u);
}

BOOST_AUTO_TEST_CASE(pageDown_clamps_at_end) {
  TB<TextBuffer> buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(2, RowIdx{10});
  buf.pageDown(RowIdx{10}, ColIdx{20}, true);
  // Should not go past the last line.
  BOOST_CHECK(buf.point().line <= 2u);
}
