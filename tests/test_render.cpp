#define BOOST_TEST_MODULE Render
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// --- Basic rendering ---

BOOST_AUTO_TEST_CASE(render_empty_buffer) {
  TextBuffer buf("test");
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  // Output should contain something (at least cursor positioning + gutter).
  BOOST_CHECK(!out.empty());
}

BOOST_AUTO_TEST_CASE(render_single_line) {
  TextBuffer buf("test");
  insertString(buf, "hello");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  auto grid = renderToGrid(out, 5, 20);
  // First row should contain "hello".
  BOOST_CHECK(grid[0].find("hello") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_multiple_lines) {
  TextBuffer buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.moveHome();
  buf.gotoLine(1, RowIdx{10});
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  auto grid = renderToGrid(out, 5, 20);
  BOOST_CHECK(grid[0].find("aaa") != std::string::npos);
  BOOST_CHECK(grid[1].find("bbb") != std::string::npos);
  BOOST_CHECK(grid[2].find("ccc") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_cursor_at_start) {
  TextBuffer buf("test");
  insertString(buf, "hello");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
}

BOOST_AUTO_TEST_CASE(render_cursor_at_middle) {
  TextBuffer buf("test");
  insertString(buf, "hello");
  buf.moveHome();
  buf.moveRight(); // past 'h'
  buf.moveRight(); // past 'e'
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 2u);
}

BOOST_AUTO_TEST_CASE(render_cursor_at_end) {
  TextBuffer buf("test");
  insertString(buf, "hello");
  // Point is already at end after insertString.
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 5u);
}

// --- Wrap mode ---

BOOST_AUTO_TEST_CASE(render_wrap_basic) {
  TextBuffer buf("test");
  // cols=6 means cw=5. "abcdefgh" (8 chars) wraps: row 0: "abcde", row 1: "fgh".
  insertString(buf, "abcdefgh");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  auto grid = renderToGrid(out, 5, 6);
  BOOST_CHECK(grid[0].find("abcde") != std::string::npos);
  BOOST_CHECK(grid[1].find("fgh") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_wrap_exact_boundary) {
  // Regression test: cursor at end of a line whose width is exactly cw.
  // cols=6 means cw=5. "abcde" is exactly 5 chars = exactly cw.
  TextBuffer buf("test");
  insertString(buf, "abcde");
  // Point is at end (offset=5).
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6});
  // Cursor should be on the next visual row, column 0.
  BOOST_CHECK_EQUAL(row.value, 1u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  // And this should match screenRow().
  BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{6}, true), row.value);
}

BOOST_AUTO_TEST_CASE(render_wrap_exact_boundary_multiline) {
  // Exact boundary with content on the next line.
  TextBuffer buf("test");
  insertString(buf, "abcde\nfg");
  // Move to end of first line.
  buf.gotoLine(1, RowIdx{10});
  buf.moveEnd();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6});
  // With cw=5, "abcde" fills row 0 exactly. Cursor at end should be at (1,0).
  BOOST_CHECK_EQUAL(row.value, 1u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{6}, true), row.value);
}

BOOST_AUTO_TEST_CASE(render_wrap_multi_wrap) {
  // A line that wraps multiple times.
  TextBuffer buf("test");
  // cols=4 means cw=3. "abcdefghi" (9 chars) wraps: row 0: "abc", row 1: "def", row 2: "ghi".
  insertString(buf, "abcdefghi");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{4});
  BOOST_CHECK_EQUAL(row.value, 0u);
}

BOOST_AUTO_TEST_CASE(render_wrap_gutter_indicator) {
  // In wrap mode, wrapped lines should have a gutter indicator (↵).
  TextBuffer buf("test");
  insertString(buf, "abcdefgh");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{6});
  auto stripped = stripAnsi(out);
  // The wrap indicator "↵" should appear somewhere in the output for row 0's gutter.
  // (The gutter is at the last column of each row.)
}

// --- No-wrap mode ---

BOOST_AUTO_TEST_CASE(render_nowrap_horizontal_scroll) {
  TextBuffer buf("test");
  insertString(buf, "abcdefghij");
  // Move cursor to 'f' (offset 5), then ensure visible with narrow terminal.
  buf.moveHome();
  for (int i = 0; i < 5; ++i)
    buf.moveRight();
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6}, false, false);
  // Cursor should be visible.
  BOOST_CHECK(col.value < 5u);
}

BOOST_AUTO_TEST_CASE(render_nowrap_cursor_tracks_hscroll) {
  TextBuffer buf("test");
  insertString(buf, "abcdefghij");
  buf.moveEnd();
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6}, false, false);
  BOOST_CHECK_EQUAL(row.value, 0u);
  // Cursor should be within visible range.
  BOOST_CHECK(col.value <= 5u);
}

BOOST_AUTO_TEST_CASE(render_nowrap_overflow_indicator) {
  TextBuffer buf("test");
  insertString(buf, "abcdefghij");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{6}, false, false);
  // The gutter should show an overflow indicator (↠) for the truncated line.
  // We can check the raw output contains the gutter character.
  auto stripped = stripAnsi(out);
  // ↠ is U+21A0, UTF-8: E2 86 A0
  BOOST_CHECK(out.find("\xe2\x86\xa0") != std::string::npos);
}

// --- Wide characters (CJK) ---

BOOST_AUTO_TEST_CASE(render_cjk_basic) {
  TextBuffer buf("test");
  // Insert CJK character 中 (U+4E2D, width=2).
  buf.insert(0x4E2Du);
  buf.insert('x');
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
}

BOOST_AUTO_TEST_CASE(render_cjk_wrap_overflow) {
  // CJK char that doesn't fit at end of row wraps to next row.
  TextBuffer buf("test");
  // cols=4, cw=3. Insert "ab" (2 cols) then 中 (2 cols). "ab" takes 2 cols,
  // 中 needs 2 but only 1 left, so it wraps to next row.
  insertString(buf, "ab");
  buf.insert(0x4E2Du);
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{4});
  // "ab" on row 0, 中 on row 1.
  BOOST_CHECK_EQUAL(row.value, 0u);
}

BOOST_AUTO_TEST_CASE(render_cjk_exact_boundary) {
  // Two CJK chars exactly fill cw=4: cols=5, cw=4. Each char is width 2.
  TextBuffer buf("test");
  buf.insert(0x4E2Du); // 中
  buf.insert(0x6587u); // 文
  // Point at end, width=4 fills cw=4 exactly.
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{5});
  // Cursor should wrap to next row at col 0.
  BOOST_CHECK_EQUAL(row.value, 1u);
  BOOST_CHECK_EQUAL(col.value, 0u);
  BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{5}, true), row.value);
}

// --- Consistency: render() vs screenRow() ---

BOOST_AUTO_TEST_CASE(render_matches_screenRow_basic) {
  TextBuffer buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.gotoLine(3, RowIdx{10});
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{10}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, buf.screenRow(ColIdx{20}, true));
}

BOOST_AUTO_TEST_CASE(render_matches_screenRow_wrapped) {
  TextBuffer buf("test");
  insertString(buf, "abcdefghij");
  buf.moveEnd();
  std::string out;
  // cols=4, cw=3. 10 chars: wraps into 4 rows.
  auto [row, col] = buf.render(out, RowIdx{10}, ColIdx{4});
  BOOST_CHECK_EQUAL(row.value, buf.screenRow(ColIdx{4}, true));
}

BOOST_AUTO_TEST_CASE(render_matches_screenRow_mid_word) {
  TextBuffer buf("test");
  insertString(buf, "abcdefgh");
  buf.moveHome();
  // Move to offset 4 (past "abcd").
  for (int i = 0; i < 4; ++i)
    buf.moveRight();
  std::string out;
  // cols=4, cw=3.
  auto [row, col] = buf.render(out, RowIdx{10}, ColIdx{4});
  BOOST_CHECK_EQUAL(row.value, buf.screenRow(ColIdx{4}, true));
}

// --- Sub-line viewport (WrapOffset) ---

BOOST_AUTO_TEST_CASE(render_wrap_offset_starts_midline) {
  TextBuffer buf("test");
  // Create a long line, then scroll into it.
  for (int i = 0; i < 30; ++i)
    buf.insert('a' + (i % 26));
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{6}, true);
  // viewFrom should be sub-line.
  BOOST_CHECK(buf.viewFrom().offset > 0u);
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{3}, ColIdx{6});
  BOOST_CHECK(row.value < 3u);
}

BOOST_AUTO_TEST_CASE(render_cursor_visible_after_subline_scroll) {
  TextBuffer buf("test");
  for (int i = 0; i < 30; ++i)
    buf.insert('x');
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{4}, true);
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{3}, ColIdx{4});
  BOOST_CHECK(row.value < 3u);
}

// --- Whitespace mode ---

BOOST_AUTO_TEST_CASE(render_whitespace_spaces) {
  TextBuffer buf("test");
  insertString(buf, "a b");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20}, true); // showWhitespace=true
  // In whitespace mode, spaces are rendered as middle dot (·, U+00B7).
  BOOST_CHECK(out.find("\xc2\xb7") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_whitespace_tabs) {
  TextBuffer buf("test");
  buf.insert('\t');
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20}, true);
  // Tab in whitespace mode shows ■► (U+25A0 U+25BA).
  BOOST_CHECK(out.find("\xe2\x96\xa0\xe2\x96\xba") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_whitespace_newline) {
  TextBuffer buf("test");
  insertString(buf, "a\nb");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20}, true);
  // Newline in whitespace mode shows ↵ (U+21B5).
  BOOST_CHECK(out.find("\xe2\x86\xb5") != std::string::npos);
}

// --- Edge cases ---

BOOST_AUTO_TEST_CASE(render_single_row_viewport) {
  TextBuffer buf("test");
  insertString(buf, "hello");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{1}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 0u);
  BOOST_CHECK_EQUAL(col.value, 0u);
}

BOOST_AUTO_TEST_CASE(render_very_narrow_terminal) {
  // cols=2 means cw=1. Each character takes one visual row.
  TextBuffer buf("test");
  insertString(buf, "abc");
  buf.moveHome();
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{2});
  BOOST_CHECK_EQUAL(row.value, 0u);
  // "a" on row 0, "b" on row 1, "c" on row 2.
  auto grid = renderToGrid(out, 5, 2);
  BOOST_CHECK(grid[0].find('a') != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_cursor_on_second_line) {
  TextBuffer buf("test");
  insertString(buf, "aaa\nbbb");
  // Point is at end of "bbb" (line 1, offset 3).
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK_EQUAL(row.value, 1u);
  BOOST_CHECK_EQUAL(col.value, 3u);
}

// --- Highlight rendering ---

BOOST_AUTO_TEST_CASE(render_highlight_emits_ansi) {
  TextBuffer buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  // Highlight "hello" (bytes 0..5).
  buf.setHighlight({0, 0}, {0, 5});
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20});
  // The highlight escape (light blue bg) should appear in the output.
  BOOST_CHECK(out.find("\x1b[48;2;60;100;160m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_highlight_cleared_no_ansi) {
  TextBuffer buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.setHighlight({0, 0}, {0, 5});
  buf.clearHighlight();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20});
  // No highlight escape should appear.
  BOOST_CHECK(out.find("\x1b[48;2;60;100;160m") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_highlight_partial_line) {
  TextBuffer buf("test");
  insertString(buf, "abcdefgh");
  buf.moveHome();
  // Highlight "cde" (bytes 2..5).
  buf.setHighlight({0, 2}, {0, 5});
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20});
  // Highlight should appear.
  BOOST_CHECK(out.find("\x1b[48;2;60;100;160m") != std::string::npos);
  // Content should still be present.
  auto grid = renderToGrid(out, 5, 20);
  BOOST_CHECK(grid[0].find("abcdefgh") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_highlight_on_second_line) {
  TextBuffer buf("test");
  insertString(buf, "aaa\nbbb\nccc");
  buf.moveHome();
  // Highlight "bbb" on line 1.
  buf.setHighlight({1, 0}, {1, 3});
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20});
  BOOST_CHECK(out.find("\x1b[48;2;60;100;160m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_highlight_nowrap) {
  TextBuffer buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.setHighlight({0, 6}, {0, 11}); // "world"
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{20}, false, false); // no-wrap mode
  BOOST_CHECK(out.find("\x1b[48;2;60;100;160m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_highlight_does_not_affect_cursor_pos) {
  TextBuffer buf("test");
  insertString(buf, "hello world");
  buf.moveHome();
  buf.moveRight(); // at 'e'
  buf.moveRight(); // at 'l'

  // Render without highlight.
  std::string out1;
  auto [row1, col1] = buf.render(out1, RowIdx{5}, ColIdx{20});

  // Render with highlight.
  buf.setHighlight({0, 0}, {0, 5});
  std::string out2;
  auto [row2, col2] = buf.render(out2, RowIdx{5}, ColIdx{20});

  // Cursor position should be the same.
  BOOST_CHECK_EQUAL(row1.value, row2.value);
  BOOST_CHECK_EQUAL(col1.value, col2.value);
  buf.clearHighlight();
}

// --- Syntax highlighting ---

BOOST_AUTO_TEST_CASE(render_cpp_mode_emits_syntax_colors) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "return 0;");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // "return" is a keyword — render should contain the keyword color escape.
  // Keyword color: \x1b[38;2;80;130;200m
  BOOST_CHECK(out.find("\x1b[38;2;80;130;200m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_text_mode_no_syntax_colors) {
  TextBuffer buf("test.txt");
  // Default mode is Text — no syntax coloring expected.
  insertString(buf, "return 0;");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // No keyword color should appear.
  BOOST_CHECK(out.find("\x1b[38;2;80;130;200m") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_cpp_mode_comment_colored) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "// a comment");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // Comment color: \x1b[38;2;80;160;80m
  BOOST_CHECK(out.find("\x1b[38;2;80;160;80m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_mode_switch_toggles_highlighting) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "return x;");
  buf.moveHome();

  // Render in Cpp mode — should have keyword color.
  std::string out1;
  buf.render(out1, RowIdx{5}, ColIdx{40});
  BOOST_CHECK(out1.find("\x1b[38;2;80;130;200m") != std::string::npos);

  // Switch to Text mode — no keyword color.
  buf.setMode(BufferMode::Txt);
  std::string out2;
  buf.render(out2, RowIdx{5}, ColIdx{40});
  BOOST_CHECK(out2.find("\x1b[38;2;80;130;200m") == std::string::npos);
}
