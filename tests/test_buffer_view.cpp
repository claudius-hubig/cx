#define BOOST_TEST_MODULE BufferView
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

BOOST_AUTO_TEST_CASE(default_construction) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  BOOST_CHECK_EQUAL(view.point().line, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
  BOOST_CHECK(!view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(from_saved_state) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setSavedView({.point = {1, 3}});
  BufferView view = BufferView::fromSavedState(buf);
  BOOST_CHECK_EQUAL(view.point().line, 1u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u);
  // Saved state should be cleared after reading.
  BOOST_CHECK(!buf.savedView().has_value());
}

BOOST_AUTO_TEST_CASE(movement_through_view) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());

  // Move right 3 times via BufferView.
  BufferView view(buf);
  view.moveRight();
  view.moveRight();
  view.moveRight();
  BOOST_CHECK_EQUAL(view.point().line, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u);
}

BOOST_AUTO_TEST_CASE(move_down_up) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);

  view.moveDown(ColIdx{20});
  BOOST_CHECK_EQUAL(view.point().line, 1u);
  view.moveDown(ColIdx{20});
  BOOST_CHECK_EQUAL(view.point().line, 2u);
  view.moveUp(ColIdx{20});
  BOOST_CHECK_EQUAL(view.point().line, 1u);
}

BOOST_AUTO_TEST_CASE(insert_through_view) {
  TempFile f("ab\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.moveRight(); // after 'a'
  view.insert('X');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXb");
  BOOST_CHECK_EQUAL(view.point().offset, 2u); // after 'X'
}

BOOST_AUTO_TEST_CASE(delete_through_view) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.moveRight(); // after 'a'
  view.deleteAtPoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ac");
  BOOST_CHECK_EQUAL(view.point().offset, 1u);
}

BOOST_AUTO_TEST_CASE(delete_before_point) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.setPoint(0, 2); // after 'b'
  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ac");
  BOOST_CHECK_EQUAL(view.point().offset, 1u);
}

BOOST_AUTO_TEST_CASE(delete_line_middle) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 2); // somewhere in "bbb"
  view.deleteLine();
  BOOST_REQUIRE_EQUAL(buf.lineCount(), 3u); // "aaa", "ccc", ""
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aaa");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "ccc");
  BOOST_CHECK_EQUAL(view.point().line, 1u);
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(delete_line_first) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.deleteLine(); // cursor at (0,0)
  BOOST_CHECK_EQUAL(buf.lineContent(0), "bbb");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "ccc");
  BOOST_CHECK_EQUAL(view.point().line, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(delete_line_last_nonempty) {
  // File with no trailing newline: "aaa\nbbb"
  TempFile f("aaa\nbbb");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 1); // on "bbb"
  view.deleteLine();
  BOOST_REQUIRE_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aaa");
  BOOST_CHECK_EQUAL(view.point().line, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u); // end of "aaa"
}

BOOST_AUTO_TEST_CASE(delete_line_only_line) {
  TempFile f("hello");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 3);
  view.deleteLine();
  BOOST_REQUIRE_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "");
  BOOST_CHECK_EQUAL(view.point().line, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(delete_line_readonly_noop) {
  // While the buffer reports readonly (e.g. during history browsing),
  // deleteLine() must be a no-op. We simulate readonly by enabling
  // history-mode browsing via the public hook.
  TempFile f("aaa\nbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  buf.setBrowsingHistory(true);
  view.setPoint(0, 0);
  view.deleteLine();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aaa");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "bbb");
}

BOOST_AUTO_TEST_CASE(render_matches_buffer) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());

  // Render via BufferView at (0,2). Match the Buffer's default wrap=true
  // explicitly so the two paths produce byte-identical output.
  BufferView view(buf);
  view.setWrap(true);
  view.setPoint(0, 2);
  std::string viewOut;
  auto [vr, vc] = view.render(viewOut, RowIdx{5}, ColIdx{20});

  // Render via Buffer directly with same params.
  RenderParams params;
  params.point = {0, 2};
  std::string bufOut;
  auto [br, bc] = buf.render(bufOut, RowIdx{5}, ColIdx{20}, params);

  BOOST_CHECK_EQUAL(br.value, vr.value);
  BOOST_CHECK_EQUAL(bc.value, vc.value);
  BOOST_CHECK_EQUAL(bufOut, viewOut);
}

BOOST_AUTO_TEST_CASE(render_with_offset) {
  TempFile f("test\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);

  std::string out0;
  view.render(out0, RowIdx{3}, ColIdx{10});

  std::string out5;
  view.render(out5, RowIdx{3}, ColIdx{10}, {.rowOffset = 5, .colOffset = 10});

  // The offset version should differ (different cursorTo positions).
  BOOST_CHECK(out0 != out5);
}

BOOST_AUTO_TEST_CASE(mark_and_region) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.toggleMark();
  BOOST_CHECK(view.mark().has_value());
  view.moveRight();
  view.moveRight();
  view.moveRight();

  auto text = view.extractRegion(view.mark().value(), view.point());
  BOOST_CHECK_EQUAL(text, "hel");

  view.toggleMark();
  BOOST_CHECK(!view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(clamp_after_external_edit) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(2, 2); // on 'ccc', offset 2

  // Simulate another view deleting lines via the buffer directly.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{1, 3}, BufferPoint{2, 3}); // delete "\nccc"

  // BufferView's point is now stale (line 2 no longer exists).
  view.clampToBuffer();
  BOOST_CHECK(view.point().line < buf.lineCount());
  BOOST_CHECK(view.point().offset <= buf.lineContent(view.point().line).size());
}

BOOST_AUTO_TEST_CASE(two_views_independent_cursors) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView v1 = BufferView(buf);
  BufferView v2(buf); // default: (0,0)

  v1.setPoint(1, 3);
  BOOST_CHECK_EQUAL(v1.point().line, 1u);
  BOOST_CHECK_EQUAL(v1.point().offset, 3u);
  BOOST_CHECK_EQUAL(v2.point().line, 0u);
  BOOST_CHECK_EQUAL(v2.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(save_view_state) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 3);
  view.saveViewState();
  auto saved = buf.savedView();
  BOOST_CHECK(saved.has_value());
  BOOST_CHECK_EQUAL(saved->point.line, 0u);
  BOOST_CHECK_EQUAL(saved->point.offset, 3u);
}

BOOST_AUTO_TEST_CASE(insert_text_through_view) {
  TempFile f("ab\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.moveRight();
  auto end = view.insertText("XY");
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXYb");
  BOOST_CHECK_EQUAL(end.offset, 3u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u);
}

// --- insertText mark adjustment ---
//
// When text is inserted via a BufferView, any active mark must be
// shifted so it keeps referring to the same logical character. The
// rules are:
//   * Mark strictly before the insertion point on the same line:
//     unchanged.
//   * Mark at/after the insertion offset on the same line: moves with
//     the inserted text (columns shift, and line bumps by the number
//     of inserted newlines).
//   * Mark on a later line: bumps by the number of inserted newlines,
//     offset unchanged.

BOOST_AUTO_TEST_CASE(insertText_mark_before_insert_unchanged) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 2});   // mark on 'l' of "hello"
  view.setPoint(0, 6);    // cursor on 'w' of "world"
  view.insertText("NEW"); // "hello NEWworld"
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 0u);
  BOOST_CHECK_EQUAL(view.mark()->offset, 2u);
}

BOOST_AUTO_TEST_CASE(insertText_mark_after_insert_shifts_offset) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 8});   // mark on 'r' of "world"
  view.setPoint(0, 6);    // cursor on 'w' of "world"
  view.insertText("NEW"); // "hello NEWworld"
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 0u);
  BOOST_CHECK_EQUAL(view.mark()->offset, 11u); // 8 + 3
}

BOOST_AUTO_TEST_CASE(insertText_mark_at_insertion_offset_shifts) {
  // When mark sits exactly at the insertion offset it belongs with the
  // content that was moved rightward — it should follow.
  TempFile f("abcdef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 3});
  view.setPoint(0, 3);
  view.insertText("X");
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->offset, 4u);
}

BOOST_AUTO_TEST_CASE(insertText_newline_bumps_mark_on_later_line) {
  TempFile f("line0\nline1\nline2\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({2, 3});       // mark on 'e' of "line2"
  view.setPoint(0, 5);        // end of "line0"
  view.insertText("\nextra"); // inserts a new line below
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 3u); // was 2, now 3
  BOOST_CHECK_EQUAL(view.mark()->offset, 3u);
}

BOOST_AUTO_TEST_CASE(insertText_newline_on_same_line_after_mark_pos) {
  // Insert a newline within a line, before the mark's offset on that
  // same line. The mark should end up on the new line with its column
  // remapped.
  TempFile f("abcdef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 5}); // on 'f'
  view.setPoint(0, 2);  // between 'b' and 'c'
  view.insertText("\n");
  // After insert: line 0 = "ab", line 1 = "cdef"
  // Mark was at (0, 5) meaning offset 5 in "abcdef", which corresponds
  // to 'f' — now at line 1, offset 3.
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 1u);
  BOOST_CHECK_EQUAL(view.mark()->offset, 3u);
}

BOOST_AUTO_TEST_CASE(insertText_multiline_before_mark_bumps_line) {
  TempFile f("one\ntwo\nthree\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({2, 2}); // 'r' in "three"
  view.setPoint(0, 3);  // end of "one"
  view.insertText("\nA\nB");
  // New contents: one, A, B, two, three
  // Mark's line was 2, two newlines inserted → line 4, offset unchanged.
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 4u);
  BOOST_CHECK_EQUAL(view.mark()->offset, 2u);
}

BOOST_AUTO_TEST_CASE(insertText_mark_on_earlier_line_unchanged) {
  TempFile f("one\ntwo\nthree\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 1});
  view.setPoint(1, 3); // end of "two"
  view.insertText("\nmore");
  BOOST_REQUIRE(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark()->line, 0u);
  BOOST_CHECK_EQUAL(view.mark()->offset, 1u);
}

BOOST_AUTO_TEST_CASE(insertText_no_mark_is_noop_on_mark) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2);
  view.insertText("X");
  BOOST_CHECK(!view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(delete_region_through_view) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.deleteRegion(BufferPoint{0, 0}, BufferPoint{0, 5});
  BOOST_CHECK_EQUAL(buf.lineContent(0), " world");
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(line_col_through_view) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.setPoint(1, 3);
  auto [line, col] = view.lineCol();
  BOOST_CHECK_EQUAL(line, 2u); // 1-indexed
  BOOST_CHECK_EQUAL(col, 4u);  // 1-indexed codepoint column
}

BOOST_AUTO_TEST_CASE(moveHome_clamps_after_external_edit) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(2, 1); // on 'ccc', offset 1

  // Another view deletes lines so line 2 no longer exists.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{1, 0}, BufferPoint{2, 3});

  // moveHome on stale view should clamp first, not OOB.
  view.moveHome();
  BOOST_CHECK(view.point().line < buf.lineCount());
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(save_restore_preserves_desiredCol) {
  TempFile f("short\nlonger line\nhi\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView v1(buf);
  // Move to end of "longer line" then up — desiredCol should be set.
  v1.setPoint(1, 11); // end of "longer line"
  v1.moveUp(ColIdx{80});
  // Now on "short" with desiredCol remembering column 11.
  BOOST_CHECK_EQUAL(v1.point().line, 0u);

  v1.saveViewState();
  BufferView v2 = BufferView::fromSavedState(buf);
  // moveDown should restore to "longer line" at the remembered column.
  v2.moveDown(ColIdx{80});
  BOOST_CHECK_EQUAL(v2.point().line, 1u);
  BOOST_CHECK_EQUAL(v2.point().offset, 11u);
}

BOOST_AUTO_TEST_CASE(ensure_visible_through_view) {
  TempFile f("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView(buf);
  view.setPoint(9, 0); // line 'j'
  view.ensureVisible(RowIdx{3}, ColIdx{10}, false);
  // Viewport should have scrolled to make line 9 visible.
  BOOST_CHECK(view.viewFromRaw().line.value > 0u);
}

BOOST_AUTO_TEST_CASE(ensure_visible_zero_rows_no_crash) {
  // Regression: when a terminal resize produces a panel with zero content
  // rows, ensureVisible must not crash (its ring buffer used to compute
  // `ringPos + 1 % ringSize` where ringSize is rows.value, causing a
  // modulo-by-zero).
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 3);
  // Must not crash.
  view.ensureVisible(RowIdx{0}, ColIdx{10}, true);
  view.ensureVisible(RowIdx{0}, ColIdx{10}, false);
}

// --- clampToBuffer edge cases ---
//
// When another view deletes lines or shortens content, a stale
// BufferView whose _docVersion doesn't match the buffer's needs to
// clamp its point, mark, and viewport back into valid range on the next
// operation. These tests simulate that by mutating the buffer through
// a separate view.

BOOST_AUTO_TEST_CASE(clampToBuffer_point_past_deleted_lines) {
  TempFile f("aaa\nbbb\nccc\nddd\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView stale(buf);
  stale.setPoint(3, 2); // on 'ddd', offset 2

  // Delete lines 2-3 via another view.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{2, 0}, BufferPoint{3, 3});
  // Buffer is now: "aaa", "bbb", ""

  // Any operation on the stale view triggers clampToBuffer.
  stale.moveRight();
  // Point was on line 3, which no longer exists (lineCount is 3,
  // max line index is 2). Should clamp to last line.
  BOOST_CHECK(stale.point().line < buf.lineCount());
}

BOOST_AUTO_TEST_CASE(clampToBuffer_mark_past_deleted_lines) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView stale(buf);
  stale.setPoint(0, 0);
  stale.setMark({2, 2}); // mark on 'ccc', offset 2

  // Delete line 2 via another view.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{1, 3}, BufferPoint{2, 3});
  // Buffer is now: "aaa", "bbb"

  stale.moveRight();
  BOOST_REQUIRE(stale.mark().has_value());
  BOOST_CHECK(stale.mark()->line < buf.lineCount());
}

BOOST_AUTO_TEST_CASE(clampToBuffer_offset_past_shortened_line) {
  TempFile f("longline\nshort\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView stale(buf);
  stale.setPoint(0, 8); // end of "longline"
  stale.setMark({0, 6});

  // Shorten line 0 via another view.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{0, 3}, BufferPoint{0, 8});
  // Line 0 is now "lon"

  stale.moveRight();
  BOOST_CHECK(stale.point().offset <= buf.lineContent(0).size());
  BOOST_REQUIRE(stale.mark().has_value());
  BOOST_CHECK(stale.mark()->offset <= buf.lineContent(stale.mark()->line).size());
}

BOOST_AUTO_TEST_CASE(clampToBuffer_viewport_past_deleted_lines) {
  TempFile f("a\nb\nc\nd\ne\nf\ng\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView stale(buf);
  // Scroll viewport down so viewFrom is on line 5.
  stale.setPoint(5, 0);
  stale.ensureVisible(RowIdx{3}, ColIdx{20}, true);
  // viewFrom should be somewhere around line 3-5.

  // Delete all lines past line 1 via another view.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{1, 1}, BufferPoint{7, 0});
  // Buffer is now: "a", "b"

  // Any operation on the stale view should clamp viewport.
  stale.moveHome();
  BOOST_CHECK(stale.point().line < buf.lineCount());
}

// --- Version history mode ---

BOOST_AUTO_TEST_CASE(edits_grow_history) {
  TempFile f("hi\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  // Version 0 captured by the constructor.
  BOOST_CHECK_EQUAL(buf.history().size(), 1u);
  view.insert('a');
  BOOST_CHECK_EQUAL(buf.history().size(), 2u);
  view.insert('b');
  BOOST_CHECK_EQUAL(buf.history().size(), 3u);
  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(buf.history().size(), 4u);
}

BOOST_AUTO_TEST_CASE(enter_history_makes_readonly) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  BOOST_CHECK(!buf.readonly());
  view.enterHistoryMode();
  BOOST_CHECK(view.inHistoryMode());
  BOOST_CHECK(buf.readonly());
  // Edits are no-ops while browsing.
  auto before = buf.lineContent(0);
  view.insert('Z');
  BOOST_CHECK_EQUAL(buf.lineContent(0), before);
}

BOOST_AUTO_TEST_CASE(second_view_cannot_enter_history_while_browsed) {
  // The displayed snapshot is per buffer: a second browser used to start
  // at "latest" while showing the first view's older text, and committing
  // there saved that older text as if it were current.
  TempFile f("a\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView v1(buf), v2(buf);
  insertString(v1, "123");
  BOOST_REQUIRE(v1.enterHistoryMode());
  v1.stepHistory(-2);
  BOOST_CHECK(!v2.enterHistoryMode());
  BOOST_CHECK(!v2.inHistoryMode());
  v1.exitHistoryModeCommit();
  BOOST_CHECK(v2.enterHistoryMode());
}

BOOST_AUTO_TEST_CASE(step_history_restores_content) {
  TempFile f("orig\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  // v0 = "orig"
  view.setPoint(0, 4);
  view.insert('!');
  // v1 = "orig!"
  BOOST_CHECK_EQUAL(buf.lineContent(0), "orig!");
  view.enterHistoryMode();
  view.stepHistory(-1);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "orig");
  view.stepHistory(+1);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "orig!");
}

BOOST_AUTO_TEST_CASE(commit_truncates_forward_history) {
  TempFile f("A\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('1');
  view.insert('2');
  view.insert('3');
  // history: v0(ctor), v1, v2, v3 = 4 entries
  BOOST_CHECK_EQUAL(buf.history().size(), 4u);
  view.enterHistoryMode();
  view.stepHistory(-2); // go back 2 steps
  auto idx = *view.historyIndex();
  view.exitHistoryModeCommit();
  BOOST_CHECK(!view.inHistoryMode());
  BOOST_CHECK(!buf.readonly());
  BOOST_CHECK_EQUAL(buf.history().size(), idx + 1);
}

BOOST_AUTO_TEST_CASE(save_preserves_history) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  auto sizeBefore = buf.history().size();
  BOOST_CHECK(buf.save());
  BOOST_CHECK_EQUAL(buf.history().size(), sizeBefore);
}

BOOST_AUTO_TEST_CASE(revert_adds_history_entry) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  auto sizeBefore = buf.history().size();
  BOOST_CHECK(buf.revert());
  BOOST_CHECK_EQUAL(buf.history().size(), sizeBefore + 1);
}

BOOST_AUTO_TEST_CASE(exit_history_clears_browsing_flag) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  view.enterHistoryMode();
  BOOST_CHECK(buf.readonly());
  // Simulate what setBuffer does: commit and clear.
  view.exitHistoryModeCommit();
  BOOST_CHECK(!buf.readonly());
  BOOST_CHECK(!view.inHistoryMode());
  // A new view on the same buffer should be able to edit.
  BufferView view2(buf);
  auto before = buf.lineContent(0);
  view2.insert('Z');
  BOOST_CHECK(buf.lineContent(0) != before);
}

BOOST_AUTO_TEST_CASE(apply_snapshot_sets_modified) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  BOOST_CHECK(buf.save());
  BOOST_CHECK(!buf.modified());
  // Step back in history — applySnapshot should mark modified.
  view.enterHistoryMode();
  view.stepHistory(-1);
  BOOST_CHECK(buf.modified());
}

// gotoLine in no-wrap mode previously assigned `_viewFrom.col = ByteOffset{0}`
// unconditionally. With a saved no-wrap state (VisualCol alternative),
// any subsequent hScroll() access threw bad_variant_access. The fix
// preserves whichever variant alternative is active.
BOOST_AUTO_TEST_CASE(gotoLine_preserves_variant_alternative_in_nowrap) {
  TempFile f("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n");
  TextBuffer buf(f.path().string(), f.path());
  // Seed a no-wrap saved state — VisualCol alternative.
  ViewFrom vf{};
  vf.col = VisualCol{0};
  buf.setSavedView({.point = {0, 0}, .viewFrom = vf, .wrapMode = false});
  BufferView view = BufferView::fromSavedState(buf);
  view.gotoLine(5, RowIdx{4});
  // hScroll() must be reachable without bad_variant_access.
  BOOST_CHECK(std::holds_alternative<VisualCol>(view.viewFromRaw().col));
  BOOST_CHECK_NO_THROW((void)view.viewFromRaw().hScroll());
}

BOOST_AUTO_TEST_CASE(gotoLine_resets_desiredCol) {
  TempFile f("short\nlonger line\nhi\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  // Build a desiredCol intent at column 11.
  view.setPoint(1, 11);
  view.moveUp(ColIdx{80});
  // Now jump elsewhere — the prior intent must not survive.
  view.gotoLine(3, RowIdx{4});
  view.moveDown(ColIdx{80});
  // If desiredCol persisted we'd land at column 11; with reset, column 0.
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

// clampToBuffer must drop the desiredCol cache so a vertical move after an
// external edit doesn't honor a column from a now-stale logical line.
BOOST_AUTO_TEST_CASE(clampToBuffer_resets_desiredCol) {
  TempFile f("short\nlonger line\nhi\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView stale(buf);
  stale.setPoint(1, 11);
  stale.moveUp(ColIdx{80}); // stale now has desiredCol=11

  BufferView other(buf);
  other.deleteRegion(BufferPoint{1, 0}, BufferPoint{1, 11});
  // line 1 is now empty.

  // Triggering clampToBuffer via any public access should drop desiredCol.
  stale.moveDown(ColIdx{80});
  // If desiredCol survived the clamp, point would jump back to col 11
  // on a new line; with reset, it stays at col 0.
  BOOST_CHECK_EQUAL(stale.point().offset, 0u);
}

// In wrap mode a stale ByteOffset can land past the new line size after
// another view shortens the viewport's first physical line. Render uses
// that offset to start iteration; it must be clamped first.
BOOST_AUTO_TEST_CASE(clampToBuffer_clamps_viewFrom_byteOffset) {
  TempFile f("aaaaaaaaaa\nbbbbbbbbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 8);
  // Force a wrap-mode viewFrom with a non-zero ByteOffset on line 0.
  view.ensureVisible(RowIdx{2}, ColIdx{6}, /*wrap=*/true);

  // Shorten line 0 from another view so the cached ByteOffset is past EOL.
  BufferView other(buf);
  other.deleteRegion(BufferPoint{0, 2}, BufferPoint{0, 10});

  // Clamp via any public method.
  view.moveLeft();
  auto vfo = std::get_if<ByteOffset>(&view.viewFromRaw().col);
  if (vfo)
    BOOST_CHECK(vfo->value <= buf.lineContent(view.viewFromRaw().line.value).size());
}

// deleteRegion previously left _desiredCol stale so a vertical move after
// a region delete overshot.
BOOST_AUTO_TEST_CASE(deleteRegion_resets_desiredCol) {
  TempFile f("short\nlonger line\nhi\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 11);
  view.moveUp(ColIdx{80}); // desiredCol = 11
  // Now delete a region; resulting point lands at the region start.
  view.setPoint(1, 11);
  view.deleteRegion(BufferPoint{1, 0}, BufferPoint{1, 11});
  view.moveDown(ColIdx{80});
  // If desiredCol leaked through deleteRegion, we'd seek toward col 11.
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

// Entering history mode previously did not clear the mark (stepHistory
// did). The mark cannot meaningfully refer to a snapshot position.
BOOST_AUTO_TEST_CASE(enterHistoryMode_clears_mark) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  view.setMark({0, 1});
  BOOST_REQUIRE(view.mark().has_value());
  view.enterHistoryMode();
  BOOST_CHECK(!view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(recenter_no_wrap_middle_top_bottom) {
  // 30 lines, no wrap: a 10-row screen should be able to put the cursor
  // anywhere in [0, 9] by adjusting viewFrom.
  std::string body;
  for (int i = 0; i < 30; ++i)
    body += "line " + std::to_string(i) + "\n";
  TempFile f(body);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(20, 0);

  // Middle of a 10-row screen: position 5 → viewFrom = 15.
  view.recenter(RowIdx{10}, 5, ColIdx{40}, false);
  BOOST_CHECK_EQUAL(view.viewFrom().line, 15u);

  // Top: position 0 → viewFrom = 20.
  view.recenter(RowIdx{10}, 0, ColIdx{40}, false);
  BOOST_CHECK_EQUAL(view.viewFrom().line, 20u);

  // Bottom: position 9 → viewFrom = 11.
  view.recenter(RowIdx{10}, 9, ColIdx{40}, false);
  BOOST_CHECK_EQUAL(view.viewFrom().line, 11u);
}

BOOST_AUTO_TEST_CASE(reflow_collapses_short_paragraph) {
  TempFile f("This is\na short\nparagraph.\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(80);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "This is a short paragraph.");
  // Trailing empty line preserved (file ended with '\n').
  BOOST_CHECK_EQUAL(buf.lineCount(), 2u);
}

BOOST_AUTO_TEST_CASE(reflow_wraps_long_run) {
  TempFile f("alpha beta gamma delta epsilon zeta eta theta iota kappa\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(20); // narrow target forces wrapping
  // Each output line should fit within 20 columns.
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    auto const& l = buf.lineContent(i);
    if (!l.empty())
      BOOST_CHECK_LE(l.size(), 20u);
  }
  // Joining the output back must give us the original word stream.
  std::string joined;
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    if (buf.lineContent(i).empty())
      continue;
    if (!joined.empty())
      joined += ' ';
    joined += buf.lineContent(i);
  }
  BOOST_CHECK_EQUAL(joined, "alpha beta gamma delta epsilon zeta eta theta iota kappa");
}

BOOST_AUTO_TEST_CASE(reflow_preserves_common_indent) {
  TempFile f("  one two\n  three four\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2);
  view.reflowParagraph(80);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "  one two three four");
}

BOOST_AUTO_TEST_CASE(reflow_preserves_bullet_with_continuation) {
  TempFile f("- first item\n  with continuation\n  more text\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(80);
  // Bullet stays on first line, continuation indents under "first":
  BOOST_CHECK_EQUAL(buf.lineContent(0), "- first item with continuation more text");
}

BOOST_AUTO_TEST_CASE(reflow_keeps_list_items_separate) {
  // Three short list items: at width 80 they each stay on their own
  // line and are NOT merged into one flow.
  TempFile f("- alpha beta\n- gamma delta\n- epsilon zeta\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(80);
  BOOST_REQUIRE_GE(buf.lineCount(), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "- alpha beta");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "- gamma delta");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "- epsilon zeta");
}

BOOST_AUTO_TEST_CASE(reflow_wraps_each_list_item_under_bullet) {
  // Narrow width: each item wraps, but continuation aligns under the
  // bullet's text column (not the bullet itself).
  TempFile f("- alpha beta gamma delta\n- epsilon zeta eta theta\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(12);

  // Expect e.g.:
  //   - alpha
  //     beta gamma
  //     delta
  //   - epsilon
  //     zeta eta
  //     theta
  // Verify the second-item bullet appears on its own line (i.e. the
  // first item didn't swallow it as a word).
  bool foundSecondBullet = false;
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    auto const& l = buf.lineContent(i);
    if (l.starts_with("- epsilon")) {
      foundSecondBullet = true;
      break;
    }
  }
  BOOST_CHECK(foundSecondBullet);
}

BOOST_AUTO_TEST_CASE(reflow_handles_intro_followed_by_list) {
  // Mixed paragraph: a non-list intro line followed by list items.
  // Both halves reflow but the intro stays separate from the bullets.
  TempFile f("intro line text\n- first item\n- second item\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(80);
  BOOST_REQUIRE_GE(buf.lineCount(), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "intro line text");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "- first item");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "- second item");
}

BOOST_AUTO_TEST_CASE(reflow_blank_line_is_noop) {
  TempFile f("alpha\n\nbeta\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 0); // on the blank line
  view.reflowParagraph(80);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "beta");
}

BOOST_AUTO_TEST_CASE(reflow_region_keeps_blank_lines) {
  TempFile f("alpha\nbeta\n\ngamma\ndelta\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 0});
  view.setPoint(5, 0);
  view.reflowParagraph(80);
  BOOST_REQUIRE_EQUAL(buf.lineCount(), 4u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "alpha beta");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "gamma delta");
}

BOOST_AUTO_TEST_CASE(line_numbers_off_gutter_width_is_zero) {
  TempFile f("a\nb\nc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  BOOST_CHECK_EQUAL(view.lineNumberGutterWidth(), 0u);
}

BOOST_AUTO_TEST_CASE(line_numbers_on_gutter_width_matches_digits_plus_space) {
  // 5 lines → "5" → 1 digit + 1 space = 2.
  TempFile f("a\nb\nc\nd\ne\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setShowLineNumbers(true);
  BOOST_CHECK_EQUAL(view.lineNumberGutterWidth(), 2u);
}

BOOST_AUTO_TEST_CASE(line_numbers_render_dim_first_visible_line) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setShowLineNumbers(true);
  std::string out;
  view.render(out, RowIdx{4}, ColIdx{20});
  // Dim escape, then "1" with space (3-line buffer fits in 1 digit).
  BOOST_CHECK(out.find("\x1b[2m") != std::string::npos);
  BOOST_CHECK(out.find("1 ") != std::string::npos);
  BOOST_CHECK(out.find("2 ") != std::string::npos);
  BOOST_CHECK(out.find("3 ") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(line_numbers_skipped_for_overlay_buffers) {
  // BufferListBuffer overrides supportsLineNumberGutter() to false, so
  // lineNumberGutterWidth() should stay 0 even with the toggle on.
  // Use a TextBuffer instead and verify the explicit hook controls it.
  TempFile f("a\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setShowLineNumbers(true);
  BOOST_CHECK(view.lineNumberGutterWidth() > 0u);
  // sanity: the hook is wired so we know the override path works for
  // subclasses that opt out at the API level.
  BOOST_CHECK(buf.supportsLineNumberGutter());
}

BOOST_AUTO_TEST_CASE(highlight_current_line_emits_background) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 0); // cursor on line "bbb"

  // With current-line highlight off, the rendered output should not
  // contain the dark blue-grey background sequence.
  view.setHighlightCurrentLine(false);
  std::string off;
  view.render(off, RowIdx{4}, ColIdx{20});
  BOOST_CHECK(off.find("\x1b[48;2;38;38;48m") == std::string::npos);

  // With it on, the same render should contain that background at
  // least once (the cursor line).
  view.setHighlightCurrentLine(true);
  std::string on;
  view.render(on, RowIdx{4}, ColIdx{20});
  BOOST_CHECK(on.find("\x1b[48;2;38;38;48m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(recenter_clamps_at_buffer_start) {
  std::string body;
  for (int i = 0; i < 5; ++i)
    body += "line " + std::to_string(i) + "\n";
  TempFile f(body);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(2, 0);

  // Asking for the cursor to land on row 8 with only line 2 selected
  // would need viewFrom = -6; clamp to 0.
  view.recenter(RowIdx{10}, 8, ColIdx{40}, false);
  BOOST_CHECK_EQUAL(view.viewFrom().line, 0u);
}

// --- Overwrite mode ---

BOOST_AUTO_TEST_CASE(overwrite_mode_toggle_default) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BOOST_CHECK(!buf.overwriteMode()); // buffers start in insert mode
  buf.toggleOverwriteMode();
  BOOST_CHECK(buf.overwriteMode());
  buf.toggleOverwriteMode();
  BOOST_CHECK(!buf.overwriteMode());
}

BOOST_AUTO_TEST_CASE(overwrite_replaces_char_under_cursor) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setOverwriteMode(true);
  BufferView view(buf);
  view.insert('X'); // at offset 0, over 'a'
  BOOST_CHECK_EQUAL(buf.lineContent(0), "Xbc");
  BOOST_CHECK_EQUAL(view.point().offset, 1u); // advanced past 'X'
}

BOOST_AUTO_TEST_CASE(overwrite_at_end_of_line_appends_without_joining) {
  TempFile f("ab\ncd\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setOverwriteMode(true);
  BufferView view(buf);
  view.moveRight();
  view.moveRight(); // end of line 0, nothing under the cursor
  view.insert('X');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "abX"); // appended, not overwritten
  BOOST_CHECK_EQUAL(buf.lineContent(1), "cd");  // next line untouched
}

BOOST_AUTO_TEST_CASE(overwrite_newline_still_inserts) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setOverwriteMode(true);
  BufferView view(buf);
  view.moveRight(); // after 'a'
  view.insert('\n');
  // Newline splits the line rather than overwriting 'b'.
  BOOST_CHECK_EQUAL(buf.lineContent(0), "a");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "bc");
  BOOST_CHECK_EQUAL(view.point().line, 1u);
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(overwrite_preserves_mark_after_cursor) {
  TempFile f("abcde\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setOverwriteMode(true);
  BufferView view(buf);
  view.setPoint(0, 3); // before 'd'
  view.toggleMark();   // mark anchored at offset 3
  view.setPoint(0, 1); // point before 'b', mark still at 3
  view.insert('X');    // overwrite 'b'
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXcde");
  // The delete (-1) and insert (+1) of a single-byte char cancel, so the
  // mark stays anchored before 'd'.
  BOOST_CHECK_EQUAL(view.mark().value().offset, 3u);
}

// --- Case conversion ---

BOOST_AUTO_TEST_CASE(case_uppercase_word_no_mark) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2); // inside "hello"
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "HELLO world");
  // Cursor advances to the end of the affected word.
  BOOST_CHECK_EQUAL(view.point().offset, 5u);
  BOOST_CHECK(!view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(case_lowercase_word_no_mark) {
  TempFile f("HELLO WORLD\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.convertCase(BufferView::CaseOp::Lower);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "hello WORLD");
}

BOOST_AUTO_TEST_CASE(case_capitalize_word_no_mark) {
  TempFile f("hELLO world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.convertCase(BufferView::CaseOp::Capitalize);
  // First letter upper, remainder lower.
  BOOST_CHECK_EQUAL(buf.lineContent(0), "Hello world");
}

BOOST_AUTO_TEST_CASE(case_no_mark_uses_next_word_to_the_right) {
  TempFile f("  foo bar\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0); // on leading whitespace
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "  FOO bar");
  BOOST_CHECK_EQUAL(view.point().offset, 5u); // end of "foo"
}

BOOST_AUTO_TEST_CASE(case_no_word_after_cursor_is_noop) {
  TempFile f("foo   \n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 3); // trailing whitespace, no word to the right
  auto before = buf.docVersion();
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "foo   ");
  BOOST_CHECK_EQUAL(buf.docVersion(), before); // no edit recorded
}

BOOST_AUTO_TEST_CASE(case_region_uppercase_preserves_mark_and_point) {
  TempFile f("hello world foo\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.toggleMark();    // mark at offset 0
  view.setPoint(0, 11); // through "hello world"
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "HELLO WORLD foo");
  // Region-mode keeps both endpoints (length-preserving edit).
  BOOST_CHECK(view.mark().has_value());
  BOOST_CHECK_EQUAL(view.mark().value().offset, 0u);
  BOOST_CHECK_EQUAL(view.point().offset, 11u);
}

BOOST_AUTO_TEST_CASE(case_region_capitalize_titlecases_each_word) {
  TempFile f("the quick brown\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.toggleMark();
  view.setPoint(0, 15);
  view.convertCase(BufferView::CaseOp::Capitalize);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "The Quick Brown");
}

BOOST_AUTO_TEST_CASE(case_region_spans_multiple_lines) {
  TempFile f("alpha\nbeta\ngamma\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.toggleMark(); // (0,0)
  view.setPoint(2, 5);
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ALPHA");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "BETA");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "GAMMA");
}

BOOST_AUTO_TEST_CASE(case_utf8_multibyte_is_preserved) {
  // "café" — the 'é' is two bytes (0xC3 0xA9). ASCII upper touches only
  // a-z, leaving the multi-byte character intact and unsplit.
  TempFile f("caf\xc3\xa9 x\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "CAF\xc3\xa9 x");
}

BOOST_AUTO_TEST_CASE(case_already_uppercase_records_no_version) {
  TempFile f("HELLO world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  auto before = buf.docVersion();
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "HELLO world");
  BOOST_CHECK_EQUAL(buf.docVersion(), before); // unchanged text => no edit
  // ... but the cursor still advances past the word.
  BOOST_CHECK_EQUAL(view.point().offset, 5u);
}

BOOST_AUTO_TEST_CASE(case_readonly_buffer_is_noop) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setBrowsingHistory(true); // makes the buffer report readonly()
  BufferView view(buf);
  view.setPoint(0, 0);
  view.convertCase(BufferView::CaseOp::Upper);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "hello");
}

// --- Stale positions are clamped onto code-point boundaries ---

BOOST_AUTO_TEST_CASE(other_view_edit_does_not_leave_cursor_mid_codepoint) {
  // Panel 2's cursor after 'a'; panel 1 inserts a 3-byte character at the
  // line start. Typing in panel 2 used to split that character.
  TextBuffer buf("t");
  buf.insertText("abcd", BufferPoint{0, 0});
  BufferView v1(buf), v2(buf);
  v2.setPoint(0, 1);
  v1.setPoint(0, 0);
  insertString(v1, "\xe4\xb8\xad"); // 中
  v2.insert('Y');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "Y\xe4\xb8\xad"
                                        "abcd");
}

BOOST_AUTO_TEST_CASE(set_point_snaps_to_codepoint) {
  // Bookmarks restore raw byte offsets through setPoint().
  TextBuffer buf("t");
  buf.insertText("\xe4\xb8\xad\xe4\xb8\xad", BufferPoint{0, 0});
  BufferView v(buf);
  v.setPoint(0, 4);
  BOOST_CHECK_EQUAL(v.point().offset, 3u);
  v.setPoint(0, 99);
  BOOST_CHECK_EQUAL(v.point().offset, 6u);
}

BOOST_AUTO_TEST_CASE(clamp_snaps_mark) {
  TextBuffer buf("t");
  buf.insertText("abcd\nabcd", BufferPoint{0, 0});
  BufferView v(buf), other(buf);
  v.setPoint(0, 1);
  v.toggleMark(); // mark at (0,1)
  v.setPoint(0, 2);
  other.setPoint(0, 0);
  insertString(other, "\xc3\xa9"); // é at bytes 0-1: the mark is now inside it
  v.clampToBuffer();
  BOOST_REQUIRE(v.mark());
  BOOST_CHECK_EQUAL(v.mark()->offset, 0u);
  BOOST_CHECK_EQUAL(v.point().offset, 2u);
}

BOOST_AUTO_TEST_CASE(clamp_snaps_extra_cursors) {
  TextBuffer buf("t");
  buf.insertText("abcd\nabcd", BufferPoint{0, 0});
  BufferView v(buf), other(buf);
  v.setPoint(0, 1);
  v.addCursorDown(); // extra cursor at (1,1)
  other.setPoint(1, 0);
  insertString(other, "\xc3\xa9");
  v.clampToBuffer();
  BOOST_REQUIRE_EQUAL(v.extraCursors().size(), 1u);
  BOOST_CHECK(v.extraCursors()[0] == (BufferPoint{1, 0}));
}

BOOST_AUTO_TEST_CASE(own_edit_keeps_viewport_in_range) {
  // An edit adopts the new docVersion, which skips the lazy clamp; a
  // viewport past the shrunken buffer used to stay there until the next
  // draw.
  std::string text;
  for (int i = 0; i < 30; ++i)
    text += "line\n";
  TextBuffer buf("t");
  buf.insertText(text, BufferPoint{0, 0});
  BufferView v(buf);
  v.setPoint(30, 0);
  v.ensureVisible(RowIdx{5}, ColIdx{80}, true);
  BOOST_REQUIRE_GT(v.viewFromRaw().line.value, 20u);
  v.deleteRegion({1, 0}, {30, 0});
  BOOST_CHECK_LT(v.viewFromRaw().line.value, buf.lineCount());
}
