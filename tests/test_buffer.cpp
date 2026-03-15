#define BOOST_TEST_MODULE Buffer
#include <boost/test/included/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/char_width.hpp"

// Helper: insert a string as UTF-8 codepoints into a buffer.
static void insertString(Buffer& buf, std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        auto [cp, len] = decodeUtf8(s.data() + i, s.size() - i);
        buf.insert(cp);
        i += len;
    }
}

// --- Insert ---

BOOST_AUTO_TEST_CASE(insert_single_char) {
    TextBuffer buf("test");
    buf.insert('A');
    BOOST_CHECK_EQUAL(buf.point(), 1u);
    BOOST_CHECK(buf.modified());
}

BOOST_AUTO_TEST_CASE(insert_at_middle) {
    TextBuffer buf("test");
    insertString(buf, "ac");
    buf.moveLeft();
    buf.insert('b');
    // Should be "abc"
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(col, 3u); // after 'b', before 'c'
}

BOOST_AUTO_TEST_CASE(insert_sets_modified) {
    TextBuffer buf("test");
    BOOST_CHECK(!buf.modified());
    buf.insert('x');
    BOOST_CHECK(buf.modified());
}

BOOST_AUTO_TEST_CASE(insert_newline_updates_lineCol) {
    TextBuffer buf("test");
    insertString(buf, "hi");
    buf.insert('\n');
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 2u);
    BOOST_CHECK_EQUAL(col, 1u);
}

// --- Delete at point ---

BOOST_AUTO_TEST_CASE(deleteAtPoint_basic) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    buf.moveHome();
    buf.deleteAtPoint(); // delete 'a'
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(col, 1u);
    BOOST_CHECK(buf.modified());
}

BOOST_AUTO_TEST_CASE(deleteAtPoint_at_end_noop) {
    TextBuffer buf("test");
    insertString(buf, "ab");
    std::size_t before = buf.point();
    buf.deleteAtPoint(); // at end — no-op
    BOOST_CHECK_EQUAL(buf.point(), before);
}

BOOST_AUTO_TEST_CASE(deleteAtPoint_utf8_full_sequence) {
    TextBuffer buf("test");
    // Insert é (U+00E9, 2 bytes: C3 A9)
    buf.insert(0x00E9u);
    buf.insert('x');
    buf.moveHome();
    buf.deleteAtPoint(); // should delete both bytes of é
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(col, 1u);
    // Point should be at 'x' which is the only remaining char
    buf.moveEnd();
    auto [line2, col2] = buf.lineCol();
    BOOST_CHECK_EQUAL(col2, 2u); // 'x' + 1-based = 2? No, col counts codepoints.
    // After deleting é, buffer has "x", point is at start, moveEnd puts at end, col=2
}

// --- Delete before point ---

BOOST_AUTO_TEST_CASE(deleteBeforePoint_basic) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    buf.deleteBeforePoint(); // delete 'c'
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(col, 3u); // "ab", point at end, col=3 (1-indexed)
}

BOOST_AUTO_TEST_CASE(deleteBeforePoint_at_start_noop) {
    TextBuffer buf("test");
    insertString(buf, "ab");
    buf.moveHome();
    std::size_t before = buf.point();
    buf.deleteBeforePoint(); // at start — no-op
    BOOST_CHECK_EQUAL(buf.point(), before);
}

BOOST_AUTO_TEST_CASE(deleteBeforePoint_utf8) {
    TextBuffer buf("test");
    // Insert é (U+00E9)
    buf.insert(0x00E9u);
    buf.deleteBeforePoint(); // should delete both bytes
    BOOST_CHECK_EQUAL(buf.point(), 0u);
}

// --- Movement ---

BOOST_AUTO_TEST_CASE(moveLeft_at_start) {
    TextBuffer buf("test");
    buf.moveLeft(); // should be no-op
    BOOST_CHECK_EQUAL(buf.point(), 0u);
}

BOOST_AUTO_TEST_CASE(moveRight_at_end) {
    TextBuffer buf("test");
    insertString(buf, "hi");
    std::size_t before = buf.point();
    buf.moveRight(); // at end — no-op
    BOOST_CHECK_EQUAL(buf.point(), before);
}

BOOST_AUTO_TEST_CASE(moveLeft_utf8_skipping) {
    TextBuffer buf("test");
    // Insert é (U+00E9)
    buf.insert(0x00E9u);
    buf.moveLeft(); // should skip back over both bytes
    BOOST_CHECK_EQUAL(buf.point(), 0u);
}

BOOST_AUTO_TEST_CASE(moveRight_utf8_skipping) {
    TextBuffer buf("test");
    buf.insert(0x00E9u);
    buf.moveHome();
    buf.moveRight(); // should advance past both bytes
    BOOST_CHECK_EQUAL(buf.point(), 2u);
}

BOOST_AUTO_TEST_CASE(moveHome_moveEnd) {
    TextBuffer buf("test");
    insertString(buf, "hello");
    buf.moveHome();
    BOOST_CHECK_EQUAL(buf.point(), 0u);
    buf.moveEnd();
    BOOST_CHECK_EQUAL(buf.point(), 5u);
}

BOOST_AUTO_TEST_CASE(moveUp_down_desiredCol) {
    TextBuffer buf("test");
    // Line 1: "abcde" (5 chars)
    // Line 2: "fg" (2 chars)
    // Line 3: "hijkl" (5 chars)
    insertString(buf, "abcde\nfg\nhijkl");
    // Point is at end of line 3 (col 6, 1-indexed)

    // Go to end of line 1 (col 6)
    buf.moveUp(); // to line 2, col clamps to end of "fg"
    buf.moveUp(); // to line 1, should restore desired col

    // Move to col 5 in line 1
    buf.moveHome();
    buf.moveRight(); // a
    buf.moveRight(); // b
    buf.moveRight(); // c
    buf.moveRight(); // d  — now at col 5

    buf.moveDown(); // line 2, "fg" only has 2 chars, clamps
    auto [line2, col2] = buf.lineCol();
    BOOST_CHECK_EQUAL(line2, 2u);
    // desired col is preserved

    buf.moveDown(); // line 3, should restore to col 5
    auto [line3, col3] = buf.lineCol();
    BOOST_CHECK_EQUAL(line3, 3u);
    BOOST_CHECK_EQUAL(col3, 5u);
}

BOOST_AUTO_TEST_CASE(moveUp_from_first_line) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    buf.moveUp(); // should go to position 0
    BOOST_CHECK_EQUAL(buf.point(), 0u);
}

BOOST_AUTO_TEST_CASE(moveDown_from_last_line) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    buf.moveHome();
    buf.moveDown(); // should go to end
    BOOST_CHECK_EQUAL(buf.point(), 3u);
}

// --- Scroll ---

BOOST_AUTO_TEST_CASE(scrollUp_at_zero) {
    TextBuffer buf("test");
    insertString(buf, "abc\ndef");
    buf.scrollUp(); // viewFrom is 0, should be no-op
    BOOST_CHECK_EQUAL(buf.viewFrom(), 0u);
}

BOOST_AUTO_TEST_CASE(scrollDown_past_newline) {
    TextBuffer buf("test");
    insertString(buf, "abc\ndef");
    buf.scrollDown(); // advance past "abc\n"
    BOOST_CHECK_EQUAL(buf.viewFrom(), 4u); // start of "def"
}

BOOST_AUTO_TEST_CASE(scrollDown_at_end) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    buf.scrollDown(); // no newline to advance past
    BOOST_CHECK_EQUAL(buf.viewFrom(), 3u);
}

// --- GotoLine ---

BOOST_AUTO_TEST_CASE(gotoLine_line1) {
    TextBuffer buf("test");
    insertString(buf, "aaa\nbbb\nccc");
    buf.gotoLine(1, RowIdx{10});
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
}

BOOST_AUTO_TEST_CASE(gotoLine_line0_becomes_1) {
    TextBuffer buf("test");
    insertString(buf, "aaa\nbbb");
    buf.gotoLine(0, RowIdx{10});
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
}

BOOST_AUTO_TEST_CASE(gotoLine_past_end) {
    TextBuffer buf("test");
    insertString(buf, "aaa\nbbb\nccc");
    buf.gotoLine(100, RowIdx{10});
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 3u); // clamped to last line
}

BOOST_AUTO_TEST_CASE(gotoLine_centers_viewFrom) {
    TextBuffer buf("test");
    // Create 20 lines
    for (int i = 0; i < 20; ++i) {
        insertString(buf, "line");
        if (i < 19) buf.insert('\n');
    }
    buf.gotoLine(15, RowIdx{10}); // center around line 15
    // viewFrom should be above line 15
    BOOST_CHECK(buf.viewFrom() > 0u);
    BOOST_CHECK(buf.viewFrom() < buf.point());
}

// --- ScreenRow ---

BOOST_AUTO_TEST_CASE(screenRow_at_viewFrom) {
    TextBuffer buf("test");
    insertString(buf, "abc\ndef");
    buf.moveHome(); // point at start of "def" (line 2, but we need to go to line 1)
    // Actually point is at end after insertString. Let's reset.
    buf.gotoLine(1, RowIdx{10});
    BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{80}), 0u);
}

BOOST_AUTO_TEST_CASE(screenRow_two_lines_down) {
    TextBuffer buf("test");
    insertString(buf, "aaa\nbbb\nccc");
    buf.gotoLine(1, RowIdx{10});
    buf.moveDown();
    buf.moveDown(); // now on line 3
    BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{80}), 2u);
}

BOOST_AUTO_TEST_CASE(screenRow_wrapping) {
    TextBuffer buf("test");
    insertString(buf, "abcdefgh"); // 8 chars
    buf.moveEnd();
    // With cols=4, "abcdefgh" wraps: row 0: "abcd", row 1: "efgh"
    BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{4}), 2u);
}

// --- LineCol ---

BOOST_AUTO_TEST_CASE(lineCol_empty) {
    TextBuffer buf("test");
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
    BOOST_CHECK_EQUAL(col, 1u);
}

BOOST_AUTO_TEST_CASE(lineCol_after_insert) {
    TextBuffer buf("test");
    insertString(buf, "abc");
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 1u);
    BOOST_CHECK_EQUAL(col, 4u); // 1-indexed, after 3 chars
}

BOOST_AUTO_TEST_CASE(lineCol_after_newline) {
    TextBuffer buf("test");
    insertString(buf, "ab\n");
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(line, 2u);
    BOOST_CHECK_EQUAL(col, 1u);
}

BOOST_AUTO_TEST_CASE(lineCol_utf8_counts_codepoints) {
    TextBuffer buf("test");
    // Insert é (U+00E9, 1 codepoint)
    buf.insert(0x00E9u);
    buf.insert('x');
    auto [line, col] = buf.lineCol();
    BOOST_CHECK_EQUAL(col, 3u); // é(1) + x(1) = 2 codepoints, 1-indexed → 3
}

// --- Readonly ---

BOOST_AUTO_TEST_CASE(readonly_ignores_insert) {
    HelpBuffer buf;
    std::size_t before = buf.point();
    buf.insert('x');
    BOOST_CHECK_EQUAL(buf.point(), before);
}

BOOST_AUTO_TEST_CASE(readonly_ignores_delete) {
    HelpBuffer buf;
    buf.deleteAtPoint();
    buf.deleteBeforePoint();
    // No crash, no modification
    BOOST_CHECK(!buf.modified());
}
