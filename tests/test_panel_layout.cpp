#define BOOST_TEST_MODULE PanelLayout
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "terminal/panel.hpp"
#include "terminal/panel_layout.hpp"
#include "test_helpers.hpp"

BOOST_AUTO_TEST_CASE(single_panel_construction) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  BOOST_CHECK_EQUAL(layout.count(), 1u);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);
  BOOST_CHECK_EQUAL(&layout.focused().buffer(), &buf);

  auto rect = layout.focused().rect();
  BOOST_CHECK_EQUAL(rect.row, 0u);
  BOOST_CHECK_EQUAL(rect.col, 0u);
  BOOST_CHECK_EQUAL(rect.rows, 24u);
  BOOST_CHECK_EQUAL(rect.cols, 80u);
}

BOOST_AUTO_TEST_CASE(panel_content_rows) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  // contentRows = rect.rows - 1 (modeline)
  BOOST_CHECK_EQUAL(layout.focused().contentRows().value, 23u);
  BOOST_CHECK_EQUAL(layout.focused().totalCols().value, 80u);
}

BOOST_AUTO_TEST_CASE(split_vertical) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* newPanel = layout.splitFocused(SplitDir::Vertical, buf);
  BOOST_REQUIRE(newPanel != nullptr);
  BOOST_CHECK_EQUAL(layout.count(), 2u);

  // Focus stays on original panel.
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);

  // Left panel gets floor(80/2) = 40 cols.
  auto leftRect = layout.focused().rect();
  BOOST_CHECK_EQUAL(leftRect.col, 0u);
  BOOST_CHECK_EQUAL(leftRect.cols, 40u);
  BOOST_CHECK_EQUAL(leftRect.rows, 24u);

  // Right panel gets remaining 40 cols.
  auto rightRect = newPanel->rect();
  BOOST_CHECK_EQUAL(rightRect.col, 40u);
  BOOST_CHECK_EQUAL(rightRect.cols, 40u);
  BOOST_CHECK_EQUAL(rightRect.rows, 24u);
}

BOOST_AUTO_TEST_CASE(split_horizontal) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* newPanel = layout.splitFocused(SplitDir::Horizontal, buf);
  BOOST_REQUIRE(newPanel != nullptr);
  BOOST_CHECK_EQUAL(layout.count(), 2u);

  // Top panel gets floor(24/2) = 12 rows.
  auto topRect = layout.focused().rect();
  BOOST_CHECK_EQUAL(topRect.row, 0u);
  BOOST_CHECK_EQUAL(topRect.rows, 12u);
  BOOST_CHECK_EQUAL(topRect.cols, 80u);

  // Bottom panel gets remaining 12 rows.
  auto bottomRect = newPanel->rect();
  BOOST_CHECK_EQUAL(bottomRect.row, 12u);
  BOOST_CHECK_EQUAL(bottomRect.rows, 12u);
  BOOST_CHECK_EQUAL(bottomRect.cols, 80u);
}

BOOST_AUTO_TEST_CASE(split_too_small_rejected) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 18);

  // 18 cols < MinPanelCols * 2 (10 * 2 = 20), so vertical split should fail.
  auto* p = layout.splitFocused(SplitDir::Vertical, buf);
  BOOST_CHECK(p == nullptr);
  BOOST_CHECK_EQUAL(layout.count(), 1u);

  // Horizontal split: 24 rows >= MinPanelRows * 2 (3 * 2 = 6), should succeed.
  p = layout.splitFocused(SplitDir::Horizontal, buf);
  BOOST_CHECK(p != nullptr);
  BOOST_CHECK_EQUAL(layout.count(), 2u);
}

BOOST_AUTO_TEST_CASE(close_panel) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  layout.splitFocused(SplitDir::Vertical, buf);
  BOOST_CHECK_EQUAL(layout.count(), 2u);

  bool closed = layout.closeFocused();
  BOOST_CHECK(closed);
  BOOST_CHECK_EQUAL(layout.count(), 1u);

  // Rect should be restored to full terminal.
  auto rect = layout.focused().rect();
  BOOST_CHECK_EQUAL(rect.cols, 80u);
  BOOST_CHECK_EQUAL(rect.rows, 24u);
}

BOOST_AUTO_TEST_CASE(close_last_panel_fails) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  bool closed = layout.closeFocused();
  BOOST_CHECK(!closed);
  BOOST_CHECK_EQUAL(layout.count(), 1u);
}

BOOST_AUTO_TEST_CASE(focus_panel) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* newPanel = layout.splitFocused(SplitDir::Vertical, buf);
  int newId = newPanel->id();

  BOOST_CHECK_EQUAL(layout.focused().id(), 1);

  bool ok = layout.focusPanel(newId);
  BOOST_CHECK(ok);
  BOOST_CHECK_EQUAL(layout.focused().id(), newId);

  // Focus non-existent panel.
  ok = layout.focusPanel(99);
  BOOST_CHECK(!ok);
  BOOST_CHECK_EQUAL(layout.focused().id(), newId); // unchanged
}

BOOST_AUTO_TEST_CASE(resize_updates_rects) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  layout.splitFocused(SplitDir::Vertical, buf);

  layout.resize(30, 100);
  auto leftRect = layout.focused().rect();
  BOOST_CHECK_EQUAL(leftRect.rows, 30u);
  BOOST_CHECK_EQUAL(leftRect.cols, 50u);
}

BOOST_AUTO_TEST_CASE(view_count_for_buffer) {
  TempFile f1("hello\n");
  TextBuffer buf1(f1.path().string(), f1.path());
  TempFile f2("world\n");
  TextBuffer buf2(f2.path().string(), f2.path());

  PanelLayout layout(buf1, 24, 80);
  BOOST_CHECK_EQUAL(layout.viewCountFor(buf1), 1u);
  BOOST_CHECK_EQUAL(layout.viewCountFor(buf2), 0u);

  // Split and show buf1 in both panels.
  layout.splitFocused(SplitDir::Vertical, buf1);
  BOOST_CHECK_EQUAL(layout.viewCountFor(buf1), 2u);
}

BOOST_AUTO_TEST_CASE(by_id_lookup) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  BOOST_CHECK(layout.byId(1) != nullptr);
  BOOST_CHECK(layout.byId(99) == nullptr);
}

BOOST_AUTO_TEST_CASE(for_each_panel) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);
  layout.splitFocused(SplitDir::Vertical, buf);

  std::vector<int> ids;
  layout.forEachPanel([&](Panel& p) { ids.push_back(p.id()); });
  BOOST_CHECK_EQUAL(ids.size(), 2u);
}

BOOST_AUTO_TEST_CASE(independent_cursors_in_split) {
  TempFile f("hello\nworld\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* newPanel = layout.splitFocused(SplitDir::Vertical, buf);

  // Move cursor in panel 1.
  layout.focused().view().setPoint(1, 3);
  BOOST_CHECK_EQUAL(layout.focused().view().point().line, 1u);
  BOOST_CHECK_EQUAL(layout.focused().view().point().offset, 3u);

  // Panel 2 should still have default position.
  BOOST_CHECK_EQUAL(newPanel->view().point().line, 0u);
  BOOST_CHECK_EQUAL(newPanel->view().point().offset, 0u);
}

BOOST_AUTO_TEST_CASE(panel_set_buffer) {
  TempFile f1("hello\n");
  TextBuffer buf1(f1.path().string(), f1.path());
  TempFile f2("world\n");
  TextBuffer buf2(f2.path().string(), f2.path());

  PanelLayout layout(buf1, 24, 80);
  BOOST_CHECK_EQUAL(&layout.focused().buffer(), &buf1);

  // Switch to buf2 with state restore.
  layout.focused().setBuffer(buf2, true);
  BOOST_CHECK_EQUAL(&layout.focused().buffer(), &buf2);
}

BOOST_AUTO_TEST_CASE(close_focused_picks_new_focus) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* p2 = layout.splitFocused(SplitDir::Vertical, buf);
  int p2id = p2->id();

  // Focus the new panel, then close it.
  layout.focusPanel(p2id);
  BOOST_CHECK_EQUAL(layout.focused().id(), p2id);

  layout.closeFocused();
  // Should have focused the remaining panel.
  BOOST_CHECK_EQUAL(layout.count(), 1u);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);
}

BOOST_AUTO_TEST_CASE(close_focuses_last_used_panel) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  // Build three panels: 1, 2, 3.
  auto* p2 = layout.splitFocused(SplitDir::Vertical, buf);
  auto* p3 = layout.splitFocused(SplitDir::Horizontal, buf);
  BOOST_REQUIRE(p2 && p3);
  BOOST_CHECK_EQUAL(layout.count(), 3u);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);

  // Visit panel 2, then panel 3. MRU history becomes [1, 2], focus 3.
  layout.focusPanel(p2->id());
  layout.focusPanel(p3->id());
  BOOST_CHECK_EQUAL(layout.focused().id(), p3->id());

  // Closing panel 3 should restore focus to panel 2 (last-used),
  // not fall back to panel 1.
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.count(), 2u);
  BOOST_CHECK_EQUAL(layout.focused().id(), p2->id());

  // Closing panel 2 now should fall back to panel 1 — the only
  // remaining history entry.
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.count(), 1u);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);
}

BOOST_AUTO_TEST_CASE(close_with_empty_history_uses_first_panel) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  // Build three panels without ever calling focusPanel — history is empty.
  auto* p2 = layout.splitFocused(SplitDir::Vertical, buf);
  auto* p3 = layout.splitFocused(SplitDir::Horizontal, buf);
  BOOST_REQUIRE(p2 && p3);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);

  // Manually focus panel 3 (history: [1]), then close it: focus should
  // return to panel 1.
  layout.focusPanel(p3->id());
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);
}

BOOST_AUTO_TEST_CASE(close_skips_stale_history_entries) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  auto* p2 = layout.splitFocused(SplitDir::Vertical, buf);
  auto* p3 = layout.splitFocused(SplitDir::Horizontal, buf);
  BOOST_REQUIRE(p2 && p3);

  // Visit 2, then 3 (focused), then back to 1. History: [2, 3].
  layout.focusPanel(p2->id());
  layout.focusPanel(p3->id());
  layout.focusPanel(1);
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);

  // Focus panel 3, so history becomes [2, 1]. Close panel 3.
  // Expected focus: panel 1 (last-used).
  layout.focusPanel(p3->id());
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.focused().id(), 1);
}

BOOST_AUTO_TEST_CASE(triple_split_and_close) {
  TempFile f("hello\n");
  TextBuffer buf(f.path().string(), f.path());
  PanelLayout layout(buf, 24, 80);

  // Split twice: panel 1 | (panel 1 top / panel 1 bottom)
  layout.splitFocused(SplitDir::Vertical, buf);   // panel 2 on right
  layout.splitFocused(SplitDir::Horizontal, buf); // panel 3 below panel 1
  BOOST_CHECK_EQUAL(layout.count(), 3u);

  // Close panel 1, should leave 2 panels.
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.count(), 2u);

  // Close again, should leave 1 panel.
  layout.closeFocused();
  BOOST_CHECK_EQUAL(layout.count(), 1u);

  // Full rect restored.
  auto rect = layout.focused().rect();
  BOOST_CHECK_EQUAL(rect.cols, 80u);
  BOOST_CHECK_EQUAL(rect.rows, 24u);
}
