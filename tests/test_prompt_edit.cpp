#define BOOST_TEST_MODULE PromptEdit
#include <boost/test/unit_test.hpp>

#include "prompt.hpp"

static Prompt withInput(std::string input, std::size_t cursor = std::string::npos) {
  Prompt p;
  p.input = std::move(input);
  p.cursor = cursor;
  return p;
}

// --- cursor defaults ---

BOOST_AUTO_TEST_CASE(default_cursor_is_at_end_of_prefilled_input) {
  auto p = withInput("name.txt");
  BOOST_CHECK_EQUAL(p.cursorPos(), 8u);
  BOOST_CHECK(p.cursorAtEnd());
}

BOOST_AUTO_TEST_CASE(cursor_follows_end_when_input_grows_externally) {
  auto p = withInput("ab");
  p.input = "abcdef"; // e.g. Tab completion
  BOOST_CHECK_EQUAL(p.cursorPos(), 6u);
}

// --- insert / delete ---

BOOST_AUTO_TEST_CASE(insert_at_cursor_mid_input) {
  auto p = withInput("helo", 3);
  p.insert("l");
  BOOST_CHECK_EQUAL(p.input, "hello");
  BOOST_CHECK_EQUAL(p.cursorPos(), 4u);
}

BOOST_AUTO_TEST_CASE(delete_before_removes_whole_code_point) {
  auto p = withInput("a\xc3\xa9z", 3); // a é z, cursor after é
  BOOST_CHECK(p.deleteBefore());
  BOOST_CHECK_EQUAL(p.input, "az");
  BOOST_CHECK_EQUAL(p.cursorPos(), 1u);
}

BOOST_AUTO_TEST_CASE(delete_before_at_start_is_noop) {
  auto p = withInput("abc", 0);
  BOOST_CHECK(!p.deleteBefore());
  BOOST_CHECK_EQUAL(p.input, "abc");
}

BOOST_AUTO_TEST_CASE(delete_at_removes_whole_code_point) {
  auto p = withInput("a\xc3\xa9z", 1);
  BOOST_CHECK(p.deleteAt());
  BOOST_CHECK_EQUAL(p.input, "az");
  BOOST_CHECK_EQUAL(p.cursorPos(), 1u);
}

BOOST_AUTO_TEST_CASE(delete_at_end_is_noop) {
  auto p = withInput("abc");
  BOOST_CHECK(!p.deleteAt());
}

BOOST_AUTO_TEST_CASE(delete_chunk_before_removes_word) {
  auto p = withInput("foo bar");
  BOOST_CHECK(p.deleteChunkBefore());
  BOOST_CHECK_EQUAL(p.input, "foo ");
  BOOST_CHECK(p.deleteChunkBefore());
  BOOST_CHECK_EQUAL(p.input, "foo");
}

BOOST_AUTO_TEST_CASE(delete_chunk_at_removes_word) {
  auto p = withInput("foo bar", 0);
  BOOST_CHECK(p.deleteChunkAt());
  BOOST_CHECK_EQUAL(p.input, " bar");
  BOOST_CHECK_EQUAL(p.cursorPos(), 0u);
}

// --- movement ---

BOOST_AUTO_TEST_CASE(left_right_step_over_code_points) {
  auto p = withInput("a\xc3\xa9");
  p.moveLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 1u);
  p.moveLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 0u);
  p.moveLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 0u);
  p.moveRight();
  p.moveRight();
  BOOST_CHECK_EQUAL(p.cursorPos(), 3u);
  p.moveRight();
  BOOST_CHECK_EQUAL(p.cursorPos(), 3u);
}

BOOST_AUTO_TEST_CASE(word_left_lands_on_word_start) {
  auto p = withInput("src/a.cpp  foo");
  p.moveWordLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 11u);
  p.moveWordLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 0u);
}

BOOST_AUTO_TEST_CASE(word_right_lands_after_word_end) {
  auto p = withInput("foo  bar", 0);
  p.moveWordRight();
  BOOST_CHECK_EQUAL(p.cursorPos(), 3u);
  p.moveWordRight();
  BOOST_CHECK_EQUAL(p.cursorPos(), 8u);
}

BOOST_AUTO_TEST_CASE(home_end) {
  auto p = withInput("abc", 1);
  p.moveHome();
  BOOST_CHECK_EQUAL(p.cursorPos(), 0u);
  p.moveEnd();
  BOOST_CHECK(p.cursorAtEnd());
}

// --- mark / region ---

BOOST_AUTO_TEST_CASE(region_spans_mark_and_cursor_in_either_order) {
  auto p = withInput("hello world", 6);
  BOOST_CHECK(!p.region());
  BOOST_CHECK(p.toggleMark());
  p.moveEnd();
  BOOST_CHECK(p.region() == std::pair(std::size_t{6}, std::size_t{11}));
  p.moveHome();
  BOOST_CHECK(p.region() == std::pair(std::size_t{0}, std::size_t{6}));
  BOOST_CHECK(!p.toggleMark());
  BOOST_CHECK(!p.region());
}

BOOST_AUTO_TEST_CASE(insert_before_mark_shifts_it) {
  auto p = withInput("abc", 2);
  p.toggleMark();
  p.moveHome();
  p.insert("xy");
  BOOST_CHECK_EQUAL(*p.mark, 4u);
  BOOST_CHECK(p.region() == std::pair(std::size_t{2}, std::size_t{4}));
}

BOOST_AUTO_TEST_CASE(erase_around_mark_collapses_it_to_erase_start) {
  auto p = withInput("abcdef", 3);
  p.toggleMark();
  p.erase(1, 5);
  BOOST_CHECK_EQUAL(p.input, "af");
  BOOST_CHECK_EQUAL(*p.mark, 1u);
}

BOOST_AUTO_TEST_CASE(delete_region_erases_and_drops_mark) {
  auto p = withInput("foo bar baz", 4);
  p.toggleMark();
  p.moveWordRight();
  BOOST_CHECK(p.deleteRegion());
  BOOST_CHECK_EQUAL(p.input, "foo  baz");
  BOOST_CHECK_EQUAL(p.cursorPos(), 4u);
  BOOST_CHECK(!p.mark);
}

BOOST_AUTO_TEST_CASE(input_replaced_drops_mark_and_moves_to_end) {
  auto p = withInput("abc", 1);
  p.toggleMark();
  p.input = "completed/path";
  p.inputReplaced();
  BOOST_CHECK(!p.mark);
  BOOST_CHECK(p.cursorAtEnd());
}

// --- combining marks ---

BOOST_AUTO_TEST_CASE(combining_mark_moves_and_deletes_with_its_base) {
  auto p = withInput("ae\xcc\x81"
                     "b"); // a, e+U+0301 (bytes 1..3), b
  p.moveLeft();
  BOOST_CHECK_EQUAL(p.cursorPos(), 4u);
  p.moveLeft(); // over e+mark in one step
  BOOST_CHECK_EQUAL(p.cursorPos(), 1u);
  p.moveRight();
  BOOST_CHECK_EQUAL(p.cursorPos(), 4u);
  BOOST_CHECK(p.deleteBefore());
  BOOST_CHECK_EQUAL(p.input, "ab");
  auto q = withInput("ae\xcc\x81"
                     "b",
                     1);
  BOOST_CHECK(q.deleteAt());
  BOOST_CHECK_EQUAL(q.input, "ab");
}
