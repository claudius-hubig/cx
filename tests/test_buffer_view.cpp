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
  buf.setSavedView({{1, 3}, {}, {}});
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

BOOST_AUTO_TEST_CASE(render_matches_buffer) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());

  // Render via BufferView at (0,2).
  BufferView view(buf);
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
  buf.save();
  BOOST_CHECK_EQUAL(buf.history().size(), sizeBefore);
}

BOOST_AUTO_TEST_CASE(revert_adds_history_entry) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('X');
  auto sizeBefore = buf.history().size();
  buf.revert();
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
  buf.save();
  BOOST_CHECK(!buf.modified());
  // Step back in history — applySnapshot should mark modified.
  view.enterHistoryMode();
  view.stepHistory(-1);
  BOOST_CHECK(buf.modified());
}
