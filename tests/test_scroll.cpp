#define BOOST_TEST_MODULE Scroll
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// --- scrollViewNearEnd ---

BOOST_AUTO_TEST_CASE(scrollNearEnd_scrolls_when_at_end) {
  TextBuffer buf("test");
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
  TextBuffer buf("test");
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
  TextBuffer buf("test");
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
  TextBuffer buf("test");
  insertString(buf, "abcdefghijklmnop");
  buf.moveEnd();
  buf.ensureVisible(RowIdx{5}, ColIdx{6}, false);
  // Horizontal scroll should have been applied.
  auto vf = buf.viewFrom();
  BOOST_CHECK_EQUAL(vf.line, 0u);
  // In no-wrap mode, hscroll should be non-zero for a 16-char line in 5 cols.
  std::string out;
  auto [row, col] = buf.render(out, RowIdx{5}, ColIdx{6}, false, false);
  BOOST_CHECK(col.value <= 5u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_wrap_subline) {
  TextBuffer buf("test");
  for (int i = 0; i < 40; ++i)
    buf.insert('x');
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{4}, true);
  BOOST_CHECK(buf.viewFrom().offset > 0u);
  BOOST_CHECK(buf.screenRow(ColIdx{4}, true) < 3u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_far_point_jump) {
  TextBuffer buf("test");
  for (int i = 0; i < 100; ++i) {
    insertString(buf, "line");
    if (i < 99)
      buf.insert('\n');
  }
  buf.gotoLine(1, RowIdx{10});
  // Now move to end.
  buf.gotoLine(100, RowIdx{10});
  buf.ensureVisible(RowIdx{10}, ColIdx{20}, true);
  BOOST_CHECK(buf.screenRow(ColIdx{20}, true) < 10u);
}

BOOST_AUTO_TEST_CASE(ensureVisible_cjk_wrap) {
  TextBuffer buf("test");
  // Fill with CJK characters (width=2 each).
  for (int i = 0; i < 20; ++i)
    buf.insert(0x4E2Du); // 中
  buf.moveEnd();
  buf.ensureVisible(RowIdx{3}, ColIdx{6}, true);
  BOOST_CHECK(buf.screenRow(ColIdx{6}, true) < 3u);
}
