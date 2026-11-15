#define BOOST_TEST_MODULE BufferListBuffer
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_view.hpp"
#include "byte_constants.hpp"
#include "test_helpers.hpp"

#include <string>
#include <vector>

BOOST_AUTO_TEST_CASE(is_readonly) {
  TB<BufferListBuffer> buf({"*1*"});
  BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(selectedName_on_file_lines) {
  std::vector<std::string> names = {"*1*", "*2*"};
  TB<BufferListBuffer> buf(names, {}, "*1*");
  // Point defaults to (0,0); "*1*" is on line 0.
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "*1*");
}

BOOST_AUTO_TEST_CASE(selectedName_empty_on_dir_headers) {
  // Two files in the same directory — produces a dir header as first line.
  std::vector<std::string> names = {"/home/user/a.txt", "/home/user/b.txt"};
  TB<BufferListBuffer> buf(names);
  // The cursor starts on the first line, which is the directory header.
  // selectedName should return empty for directory headers.
  BOOST_CHECK(buf.selectedName(buf.point()).empty());
}

BOOST_AUTO_TEST_CASE(refresh_rebuilds) {
  TB<BufferListBuffer> buf({"*1*"});
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "*1*");

  // refresh() returns a ViewHint indicating where the cursor should land;
  // callers apply it to the view explicitly.
  auto hint = buf.refresh({"*2*", "*3*"}, {}, "*3*");
  buf.setPoint(hint.point.line, hint.point.offset);
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "*3*");
}

BOOST_AUTO_TEST_CASE(startName_positions_cursor) {
  std::vector<std::string> names = {"aaa", "bbb", "ccc"};
  TB<BufferListBuffer> buf(names, {}, "bbb");
  auto hint = buf.refresh(names, {}, "bbb");
  buf.setPoint(hint.point.line, hint.point.offset);
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "bbb");
}

BOOST_AUTO_TEST_CASE(constructor_saves_view_hint) {
  // Constructor should save the ViewHint via setSavedView so that
  // fromSavedState() picks up the startName cursor position.
  std::vector<std::string> names = {"aaa", "bbb", "ccc"};
  BufferListBuffer blb(names, {}, "bbb");
  auto saved = blb.savedView();
  BOOST_REQUIRE(saved.has_value());
  BOOST_CHECK_EQUAL(saved->point.line, 1u); // "bbb" is on line 1
  BOOST_CHECK_EQUAL(saved->point.offset, 0u);
}

BOOST_AUTO_TEST_CASE(from_saved_state_lands_on_startName) {
  // End-to-end: constructor + fromSavedState should land on startName.
  std::vector<std::string> names = {"aaa", "bbb", "ccc"};
  BufferListBuffer blb(names, {}, "bbb");
  BufferView view = BufferView::fromSavedState(blb);
  BOOST_CHECK_EQUAL(blb.selectedName(view.point()), "bbb");
}

BOOST_AUTO_TEST_CASE(trie_collapses_single_subdirs) {
  // /a/b/c/file.txt should collapse /a/b/c/ into one line
  std::vector<std::string> names = {"/a/b/c/file.txt"};
  TB<BufferListBuffer> buf(names);
  // Should be a single file line, selected
  auto sel = buf.selectedName(buf.point());
  BOOST_CHECK_EQUAL(sel, "/a/b/c/file.txt");
}

BOOST_AUTO_TEST_CASE(dir_buffer_merges_into_trie) {
  // A DirBuffer for /home/user should appear as a selectable directory
  // header rather than a duplicate leaf entry.
  std::vector<std::string> names = {"/home/user/a.txt", "/home/user/b.txt"};
  std::vector<std::string> dirNames = {"/home/user"};
  TB<BufferListBuffer> buf(names, dirNames);

  // First line is the directory header — should now be selectable.
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "/home/user");
}

BOOST_AUTO_TEST_CASE(dir_buffer_prevents_collapse) {
  // A DirBuffer at an intermediate node should prevent collapsing.
  std::vector<std::string> names = {"/a/b/c/file.txt"};
  std::vector<std::string> dirNames = {"/a/b"};
  TB<BufferListBuffer> buf(names, dirNames);

  // /a/b/ should be a separate selectable header (not collapsed with /a/b/c/).
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "/a/b");
}

BOOST_AUTO_TEST_CASE(standalone_dir_buffer_without_files) {
  // A DirBuffer with no files under it should still appear.
  std::vector<std::string> names = {"*1*"};
  std::vector<std::string> dirNames = {"/home/project"};
  TB<BufferListBuffer> buf(names, dirNames);

  // *1* is first (non-path), then the dir buffer.
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "*1*");
  buf.movePhysicalDown();
  BOOST_CHECK_EQUAL(buf.selectedName(buf.point()), "/home/project");
}

BOOST_AUTO_TEST_CASE(render_clamps_long_path_to_panel_width) {
  // Rows were written unclipped, unlike those of the directory and grep
  // buffers: a long path ran past the panel into the pane beside it, or
  // wrapped at the edge of the terminal.
  std::string longName = "/a/" + std::string(120, 'x') + ".txt";
  TB<BufferListBuffer> buf(std::vector<std::string>{longName, "*1*"});

  std::string narrow;
  buf.render(narrow, RowIdx{10}, ColIdx{40}, renderParams(buf));
  BOOST_CHECK(narrow.find(std::string(120, 'x')) == std::string::npos);
  // Nothing is drawn in or beyond the gutter column except the gutter.
  for (auto const& row : renderToGrid(narrow, 10, 60))
    BOOST_CHECK_EQUAL(row.substr(39), std::string(21, ' '));

  std::string wide;
  buf.render(wide, RowIdx{10}, ColIdx{300}, renderParams(buf));
  BOOST_CHECK(wide.find(std::string(120, 'x')) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(render_resets_style_cut_by_the_clamp) {
  // A control byte in a name is shown as a coloured escape; when the
  // clamp cuts inside it the colour must not leak into the rest of the row.
  std::string name = "/a/" + std::string(30, 'y') + "\x01\x01\x01\x01.txt";
  TB<BufferListBuffer> buf(std::vector<std::string>{name, "/a/other.txt"});
  std::string out;
  buf.render(out, RowIdx{10}, ColIdx{40}, renderParams(buf));
  auto colour = out.rfind(ansi::CtrlColor);
  BOOST_REQUIRE(colour != std::string::npos);
  auto reset = out.find(ansi::Reset, colour);
  auto erase = out.find(ansi::EraseLine, colour);
  BOOST_REQUIRE(reset != std::string::npos);
  BOOST_CHECK_LT(reset, erase);
}
