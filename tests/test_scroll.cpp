#define BOOST_TEST_MODULE Scroll
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// --- scrollViewNearEnd ---

BOOST_AUTO_TEST_CASE(scrollNearEnd_scrolls_when_at_end) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i) {
    insertString(buf, "line");
    if (i < 19)
      buf.insert('\n');
  }
  buf.moveEnd();
  buf.ensureVisible(RowIdx{10}, ColIdx{20}, true);
  auto viewBefore = buf.viewFrom();
  buf.scrollViewNearEnd(RowIdx{10});
  // Should scroll down (or stay if already past limit).
  BOOST_CHECK(buf.viewFrom() >= viewBefore);
}

BOOST_AUTO_TEST_CASE(scrollNearEnd_stops_at_limit) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i) {
    insertString(buf, "line");
    if (i < 19)
      buf.insert('\n');
  }
  buf.moveEnd();
  // Scroll many times — should stop at the limit.
  for (int i = 0; i < 30; ++i)
    buf.scrollViewNearEnd(RowIdx{10});
  // viewFrom should be around lines - half.
  BOOST_CHECK(buf.viewFrom().line <= 20u);
}

BOOST_AUTO_TEST_CASE(scrollNearEnd_noop_when_past_limit) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i) {
    insertString(buf, "line");
    if (i < 19)
      buf.insert('\n');
  }
  // Set viewFromLine past the limit (lines - half = 20 - 5 = 15).
  buf.gotoLine(18, RowIdx{10}); // viewFromLine = 17 - 5 = 12... let's just scroll a lot.
  // Scroll until we're past the limit.
  for (int i = 0; i < 20; ++i)
    buf.scrollDown();
  auto viewBefore = buf.viewFrom();
  buf.scrollViewNearEnd(RowIdx{10});
  // Already past limit — should not scroll.
  BOOST_CHECK(buf.viewFrom() == viewBefore);
}

// --- ensureVisible ---

BOOST_AUTO_TEST_CASE(ensureVisible_horizontal_scroll) {
  TB<TextBuffer> buf("test");
  insertString(buf, "abcdefghijklmnop");
  buf.moveEnd();
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  // Horizontal scroll should have been applied.
  auto vf = buf.viewFrom();
  BOOST_CHECK_EQUAL(vf.line, 0u);
  // In no-wrap mode, hscroll should be non-zero for a 16-char line in 5 cols.
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6}, renderParams(buf, false, false));
  BOOST_CHECK(col.value <= 5u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_wrap_subline) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 40; ++i)
    buf.insert('x');
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{4}, true);
  BOOST_CHECK(buf.viewFrom().offset > 0u);
  BOOST_CHECK(buf.screenRow(ColIdx{4}, buf.point(), buf.viewFromRaw(), true) < 3u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_one_row_below_smooth_scrolls_not_centers) {
  // Regression: when point is exactly one visual row below the viewport
  // bottom — the natural state after a single Down keypress past
  // unwrapped content — the "far below the viewport" branch in
  // ensureVisible used to fire (>= rather than >) and jump the viewport
  // to a centred position instead of advancing it by one row.
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 30; ++i) {
    insertString(buf, "abc"); // short, won't wrap at cw=19
    if (i < 29)
      buf.insert('\n');
  }
  buf.setPoint(10, 0); // viewFrom stays at line 0; point is rows below
  BOOST_REQUIRE_EQUAL(buf.viewFrom().line, 0u);

  buf.ensureVisible(RowIdx{10}, ColIdx{20}, true);

  // Smooth one-row scroll: viewFrom advances by one. Pre-fix this was 5
  // (rows/2, the centred position).
  BOOST_CHECK_EQUAL(buf.viewFrom().line, 1u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_far_point_jump) {
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 100; ++i) {
    insertString(buf, "line");
    if (i < 99)
      buf.insert('\n');
  }
  buf.gotoLine(1, RowIdx{10});
  // Now move to end.
  buf.gotoLine(100, RowIdx{10});
  buf.ensureVisible(RowIdx{10}, ColIdx{20}, true);
  BOOST_CHECK(buf.screenRow(ColIdx{20}, buf.point(), buf.viewFromRaw(), true) < 10u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_cjk_wrap) {
  TB<TextBuffer> buf("test");
  // Fill with CJK characters (width=2 each).
  for (int i = 0; i < 20; ++i)
    buf.insert(0x4E2Du); // 中
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{6}, true);
  BOOST_CHECK(buf.screenRow(ColIdx{6}, buf.point(), buf.viewFromRaw(), true) < 3u);
}

// --- no-wrap mode: scrollDown/scrollUp must preserve horizontal scroll ---

BOOST_AUTO_TEST_CASE(scrollDown_preserves_hscroll_in_nowrap) {
  // Regression for B2: scrollDown unconditionally reset _viewFrom.col to
  // ByteOffset{0}, clobbering the VisualCol variant used for horizontal
  // scrolling.
  TB<TextBuffer> buf("test");
  insertString(buf, "abcdefghijklmnop\nqrstuvwxyz0123456789\nsecond line\n");
  buf.setPoint(0, 15); // far-right position on line 0
  // Apply no-wrap mode ensureVisible to set a non-zero VisualCol hScroll.
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  auto vfBefore = buf.viewFromRaw();
  BOOST_REQUIRE(std::holds_alternative<VisualCol>(vfBefore.col));
  BOOST_REQUIRE(vfBefore.hScroll() > 0u);
  std::size_t hscrollBefore = vfBefore.hScroll();

  buf.scrollDown();
  auto vfAfter = buf.viewFromRaw();
  // Must still be VisualCol, and the horizontal scroll position preserved.
  BOOST_CHECK(std::holds_alternative<VisualCol>(vfAfter.col));
  BOOST_CHECK_EQUAL(vfAfter.hScroll(), hscrollBefore);
  BOOST_CHECK_EQUAL(vfAfter.line.value, 1u);
}

BOOST_AUTO_TEST_CASE(scrollUp_preserves_hscroll_in_nowrap) {
  // Regression for B2 (scrollUp half).
  TB<TextBuffer> buf("test");
  insertString(buf, "abcdefghijklmnop\nqrstuvwxyz0123456789\nsecond line\n");
  buf.setPoint(1, 18); // far-right position on line 1
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  // Drop the cursor to line 1 via scrollDown, then move point back to line 1.
  // Ensure _viewFrom.line == 1 with a non-zero hScroll.
  auto vfBefore = buf.viewFromRaw();
  BOOST_REQUIRE(std::holds_alternative<VisualCol>(vfBefore.col));
  BOOST_REQUIRE(vfBefore.hScroll() > 0u);
  std::size_t hscrollBefore = vfBefore.hScroll();
  // Scroll the viewport down and then up — both must preserve hScroll.
  buf.scrollDown();
  buf.scrollUp(false);
  auto vfAfter = buf.viewFromRaw();
  BOOST_CHECK(std::holds_alternative<VisualCol>(vfAfter.col));
  BOOST_CHECK_EQUAL(vfAfter.hScroll(), hscrollBefore);
}

BOOST_AUTO_TEST_CASE(pageDown_clamp_preserves_hscroll_in_nowrap) {
  // Regression for B3: pageDown's viewport clamp forced ByteOffset{0},
  // destroying the VisualCol variant.
  TB<TextBuffer> buf("test");
  for (int i = 0; i < 20; ++i)
    insertString(buf, "abcdefghijklmnop\n");
  buf.gotoLine(20, RowIdx{6});
  buf.setPoint(19, 15);
  buf.ensureVisible(RowIdx{6}, ColIdx{6}, false);
  auto vfBefore = buf.viewFromRaw();
  BOOST_REQUIRE(std::holds_alternative<VisualCol>(vfBefore.col));
  BOOST_REQUIRE(vfBefore.hScroll() > 0u);
  std::size_t hscrollBefore = vfBefore.hScroll();

  // pageDown at the end should trigger the clamp branch.
  buf.pageDown(RowIdx{6}, ColIdx{6}, false);
  auto vfAfter = buf.viewFromRaw();
  BOOST_CHECK(std::holds_alternative<VisualCol>(vfAfter.col));
  BOOST_CHECK_EQUAL(vfAfter.hScroll(), hscrollBefore);
}
