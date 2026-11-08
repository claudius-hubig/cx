#define BOOST_TEST_MODULE CommentToggle
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "tests/test_helpers.hpp"

#include <string>

namespace {

// Build a TB<TextBuffer> populated with the given lines (joined by '\n').
auto makeBuf(std::initializer_list<std::string_view> lines) {
  auto buf = std::make_unique<TB<TextBuffer>>("*test*");
  bool first = true;
  for (auto l : lines) {
    if (!first)
      buf->view.insert('\n');
    insertString(buf->view, l);
    first = false;
  }
  return buf;
}

std::string allLines(Buffer const& b) {
  std::string out;
  for (std::size_t i = 0; i < b.lineCount(); ++i) {
    if (i)
      out += '\n';
    out += b.lineContent(i);
  }
  return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(toggle_adds_prefix_to_single_line) {
  auto b = makeBuf({"int x = 1;"});
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "// int x = 1;");
}

BOOST_AUTO_TEST_CASE(toggle_round_trips_to_original) {
  auto b = makeBuf({"int x = 1;"});
  b->view.toggleComment("// ");
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "int x = 1;");
}

BOOST_AUTO_TEST_CASE(toggle_preserves_indent) {
  auto b = makeBuf({"    if (x) y();"});
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "    // if (x) y();");
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "    if (x) y();");
}

BOOST_AUTO_TEST_CASE(toggle_skips_blank_lines_when_commenting) {
  auto b = makeBuf({"a", "", "b"});
  b->view.setMark({0, 0});
  b->view.setPoint(2, 1);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(allLines(*b), "// a\n\n// b");
}

BOOST_AUTO_TEST_CASE(toggle_uncomments_when_all_nonblank_already_commented) {
  auto b = makeBuf({"// a", "// b", "// c"});
  b->view.setMark({0, 0});
  b->view.setPoint(2, 4);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(allLines(*b), "a\nb\nc");
}

BOOST_AUTO_TEST_CASE(toggle_comments_when_some_lines_uncommented) {
  // Mixed: any uncommented line forces "comment all" semantics. Lines
  // that already had the prefix get a second prefix.
  auto b = makeBuf({"// a", "b", "c"});
  b->view.setMark({0, 0});
  b->view.setPoint(2, 1);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(allLines(*b), "// // a\n// b\n// c");
}

BOOST_AUTO_TEST_CASE(toggle_with_mark_at_col_0_excludes_trailing_line) {
  // Region (0,0)..(2,0) covers lines 0 and 1 only — line 2 is just
  // where the cursor sits when the user did "select two lines".
  auto b = makeBuf({"a", "b", "c"});
  b->view.setMark({0, 0});
  b->view.setPoint(2, 0);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(allLines(*b), "// a\n// b\nc");
}

BOOST_AUTO_TEST_CASE(toggle_no_op_on_buffer_of_only_blank_lines) {
  auto b = makeBuf({"", "   ", ""});
  b->view.setMark({0, 0});
  b->view.setPoint(2, 0);
  auto v0 = b->docVersion();
  b->view.toggleComment("// ");
  // No non-blank line means no edit and no version bump.
  BOOST_CHECK_EQUAL(b->docVersion(), v0);
  BOOST_CHECK_EQUAL(allLines(*b), "\n   \n");
}

BOOST_AUTO_TEST_CASE(toggle_readonly_is_noop) {
  // Scratch + setBrowsingHistory simulates a readonly buffer.
  auto b = makeBuf({"hello"});
  b->setBrowsingHistory(true);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "hello");
}

// --- Point and mark follow the text across the prefix edit ---

BOOST_AUTO_TEST_CASE(toggle_comment_shifts_point_by_prefix) {
  auto b = makeBuf({"  abc"});
  b->view.setPoint(0, 3); // before 'b'
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "  // abc");
  BOOST_CHECK_EQUAL(b->view.point().offset, 6u);
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "  abc");
  BOOST_CHECK_EQUAL(b->view.point().offset, 3u);
}

BOOST_AUTO_TEST_CASE(toggle_comment_leaves_point_in_indent_alone) {
  auto b = makeBuf({"    x"});
  b->view.setPoint(0, 1);
  b->view.toggleComment("# ");
  BOOST_CHECK_EQUAL(b->view.point().offset, 1u);
}

BOOST_AUTO_TEST_CASE(uncomment_moves_point_inside_prefix_to_its_start) {
  auto b = makeBuf({"  // x"});
  b->view.setPoint(0, 4); // between "//" and " "
  b->view.toggleComment("// ");
  BOOST_CHECK_EQUAL(b->lineContent(0), "  x");
  BOOST_CHECK_EQUAL(b->view.point().offset, 2u);
}

BOOST_AUTO_TEST_CASE(toggle_comment_keeps_point_on_code_point_boundary) {
  // The cursor used to keep its byte offset, landing inside "é" so the next
  // keystroke wrote invalid UTF-8.
  auto b = makeBuf({"\xc3\xa9\xc3\xa9x"});
  b->view.setPoint(0, 4); // after the second é
  b->view.toggleComment("// ");
  b->view.insert('Y');
  BOOST_CHECK_EQUAL(b->lineContent(0), "// \xc3\xa9\xc3\xa9Yx");
}

BOOST_AUTO_TEST_CASE(toggle_comment_shifts_mark_on_every_line) {
  auto b = makeBuf({"ab", "cd", "ef"});
  b->view.setPoint(0, 1);
  b->view.toggleMark();
  b->view.setPoint(1, 1);
  b->view.toggleComment("# ");
  BOOST_CHECK_EQUAL(allLines(*b), "# ab\n# cd\nef");
  BOOST_REQUIRE(b->view.mark());
  BOOST_CHECK(*b->view.mark() == (BufferPoint{0, 3}));
  BOOST_CHECK(b->view.point() == (BufferPoint{1, 3}));
}
