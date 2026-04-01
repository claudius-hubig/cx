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

BOOST_AUTO_TEST_CASE(from_buffer_state) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setPoint(1, 3);
  BufferView view = BufferView::fromBufferState(buf);
  BOOST_CHECK_EQUAL(view.point().line, 1u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u);
}

BOOST_AUTO_TEST_CASE(movement_matches_buffer) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());

  // Move right 3 times via Buffer.
  buf.setPoint(0, 0);
  buf.moveRight();
  buf.moveRight();
  buf.moveRight();
  auto bufPt = buf.point();

  // Move right 3 times via BufferView from the same start.
  buf.setPoint(0, 0);
  BufferView view = BufferView::fromBufferState(buf);
  view.moveRight();
  view.moveRight();
  view.moveRight();
  BOOST_CHECK_EQUAL(view.point().line, bufPt.line);
  BOOST_CHECK_EQUAL(view.point().offset, bufPt.offset);
}

BOOST_AUTO_TEST_CASE(move_down_up) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);

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
  BufferView view = BufferView::fromBufferState(buf);
  view.moveRight(); // after 'a'
  view.insert('X');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXb");
  BOOST_CHECK_EQUAL(view.point().offset, 2u); // after 'X'
}

BOOST_AUTO_TEST_CASE(delete_through_view) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.moveRight(); // after 'a'
  view.deleteAtPoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ac");
  BOOST_CHECK_EQUAL(view.point().offset, 1u);
}

BOOST_AUTO_TEST_CASE(delete_before_point) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.setPoint(0, 2); // after 'b'
  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "ac");
  BOOST_CHECK_EQUAL(view.point().offset, 1u);
}

BOOST_AUTO_TEST_CASE(render_matches_buffer) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setPoint(0, 2);

  // Render via Buffer.
  std::string bufOut;
  auto [br, bc] = buf.render(bufOut, RowIdx{5}, ColIdx{20});

  // Render via BufferView from the same state.
  buf.setPoint(0, 2);
  BufferView view = BufferView::fromBufferState(buf);
  std::string viewOut;
  auto [vr, vc] = view.render(viewOut, RowIdx{5}, ColIdx{20});

  BOOST_CHECK_EQUAL(br.value, vr.value);
  BOOST_CHECK_EQUAL(bc.value, vc.value);
  BOOST_CHECK_EQUAL(bufOut, viewOut);
}

BOOST_AUTO_TEST_CASE(render_with_offset) {
  TempFile f("test\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);

  std::string out0;
  view.render(out0, RowIdx{3}, ColIdx{10}, false, true, 0, 0);

  std::string out5;
  view.render(out5, RowIdx{3}, ColIdx{10}, false, true, 5, 10);

  // The offset version should differ (different cursorTo positions).
  BOOST_CHECK(out0 != out5);
}

BOOST_AUTO_TEST_CASE(mark_and_region) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
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
  BufferView view = BufferView::fromBufferState(buf);
  view.setPoint(2, 2); // on 'ccc', offset 2

  // Simulate another view deleting lines via the buffer directly.
  buf.setPoint(2, 0);
  buf.deleteRegion(BufferPoint{1, 3}, BufferPoint{2, 3}); // delete "\nccc"

  // BufferView's point is now stale (line 2 no longer exists).
  view.clampToBuffer();
  BOOST_CHECK(view.point().line < buf.lineCount());
  BOOST_CHECK(view.point().offset <= buf.lineContent(view.point().line).size());
}

BOOST_AUTO_TEST_CASE(two_views_independent_cursors) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView v1 = BufferView::fromBufferState(buf);
  BufferView v2(buf); // default: (0,0)

  v1.setPoint(1, 3);
  BOOST_CHECK_EQUAL(v1.point().line, 1u);
  BOOST_CHECK_EQUAL(v1.point().offset, 3u);
  BOOST_CHECK_EQUAL(v2.point().line, 0u);
  BOOST_CHECK_EQUAL(v2.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(save_to_buffer) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.setPoint(0, 3);
  view.saveToBuffer();
  BOOST_CHECK_EQUAL(buf.point().line, 0u);
  BOOST_CHECK_EQUAL(buf.point().offset, 3u);
}

BOOST_AUTO_TEST_CASE(insert_text_through_view) {
  TempFile f("ab\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.moveRight();
  auto end = view.insertText("XY");
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXYb");
  BOOST_CHECK_EQUAL(end.offset, 3u);
  BOOST_CHECK_EQUAL(view.point().offset, 3u);
}

BOOST_AUTO_TEST_CASE(delete_region_through_view) {
  TempFile f("hello world\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.deleteRegion(BufferPoint{0, 0}, BufferPoint{0, 5});
  BOOST_CHECK_EQUAL(buf.lineContent(0), " world");
  BOOST_CHECK_EQUAL(view.point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(line_col_through_view) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.setPoint(1, 3);
  auto [line, col] = view.lineCol();
  BOOST_CHECK_EQUAL(line, 2u); // 1-indexed
  BOOST_CHECK_EQUAL(col, 4u);  // 1-indexed codepoint column
}

BOOST_AUTO_TEST_CASE(ensure_visible_through_view) {
  TempFile f("a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view = BufferView::fromBufferState(buf);
  view.setPoint(9, 0); // line 'j'
  view.ensureVisible(RowIdx{3}, ColIdx{10}, false);
  // Viewport should have scrolled to make line 9 visible.
  BOOST_CHECK(view.viewFromRaw().line.value > 0u);
}
