#define BOOST_TEST_MODULE Replace
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "replace.hpp"

#include <memory>
#include <string>

namespace {

// Construct an in-memory TextBuffer (no path) populated line-by-line.
// Returned by unique_ptr because Buffer disables move/copy.
std::unique_ptr<TextBuffer> makeBuffer(std::initializer_list<std::string_view> lines) {
  auto buf = std::make_unique<TextBuffer>("*test*");
  BufferPoint p{0, 0};
  bool first = true;
  for (auto line : lines) {
    if (!first)
      p = buf->insertAt('\n', p);
    p = buf->insertText(line, p);
    first = false;
  }
  return buf;
}

std::string allLines(Buffer const& buf) {
  std::string out;
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    if (i)
      out += '\n';
    out += buf.lineContent(i);
  }
  return out;
}

} // namespace

// ----- findNextMatchInBuffer -----

BOOST_AUTO_TEST_CASE(findNext_returns_first_match_on_start_line) {
  auto bufPtr = makeBuffer({"hello world", "second line"});
  FindFlags f("world");
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "world", f);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.line, 0u);
  BOOST_CHECK_EQUAL(m->first.offset, 6u);
  BOOST_CHECK_EQUAL(m->second.offset, 11u);
}

BOOST_AUTO_TEST_CASE(findNext_skips_to_later_line) {
  auto bufPtr = makeBuffer({"alpha", "beta gamma", "delta"});
  FindFlags f("gamma");
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "gamma", f);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.line, 1u);
  BOOST_CHECK_EQUAL(m->first.offset, 5u);
}

BOOST_AUTO_TEST_CASE(findNext_respects_start_offset) {
  auto bufPtr = makeBuffer({"foo foo foo"});
  FindFlags f("foo");
  auto m = findNextMatchInBuffer(*bufPtr, {0, 4}, "foo", f);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.offset, 4u);
}

BOOST_AUTO_TEST_CASE(findNext_no_wrap_returns_nullopt) {
  auto bufPtr = makeBuffer({"alpha", "beta"});
  FindFlags f("alpha");
  auto m = findNextMatchInBuffer(*bufPtr, {1, 0}, "alpha", f);
  BOOST_CHECK(!m);
}

BOOST_AUTO_TEST_CASE(findNext_smart_skip_digits) {
  auto bufPtr = makeBuffer({"a1b2c"});
  FindFlags f("abc");
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "abc", f);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.offset, 0u);
  BOOST_CHECK_EQUAL(m->second.offset, 5u);
}

// ----- replaceAllForward -----

BOOST_AUTO_TEST_CASE(replaceAll_single_line_multiple_matches) {
  auto bufPtr = makeBuffer({"foo foo foo"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "foo", "bar");
  BOOST_CHECK_EQUAL(n, 3u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "bar bar bar");
}

BOOST_AUTO_TEST_CASE(replaceAll_starts_from_given_point) {
  auto bufPtr = makeBuffer({"foo foo foo"});
  auto n = replaceAllForward(*bufPtr, {0, 4}, "foo", "bar");
  BOOST_CHECK_EQUAL(n, 2u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "foo bar bar");
}

BOOST_AUTO_TEST_CASE(replaceAll_across_lines) {
  auto bufPtr = makeBuffer({"foo line", "another foo here", "trailing foo"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "foo", "bar");
  BOOST_CHECK_EQUAL(n, 3u);
  BOOST_CHECK_EQUAL(allLines(*bufPtr), "bar line\nanother bar here\ntrailing bar");
}

BOOST_AUTO_TEST_CASE(replaceAll_no_match_no_change) {
  auto bufPtr = makeBuffer({"hello world"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "zzz", "xyz");
  BOOST_CHECK_EQUAL(n, 0u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "hello world");
}

BOOST_AUTO_TEST_CASE(replaceAll_replacement_contains_pattern_terminates) {
  // Replacing "a" with "aa" must not loop: the inserted text starts past
  // the already-replaced region.
  auto bufPtr = makeBuffer({"a a a"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "a", "aa");
  BOOST_CHECK_EQUAL(n, 3u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "aa aa aa");
}

BOOST_AUTO_TEST_CASE(replaceAll_replacement_with_newline_inserts_lines) {
  auto bufPtr = makeBuffer({"foo bar"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "bar", "x\ny");
  BOOST_CHECK_EQUAL(n, 1u);
  BOOST_CHECK_EQUAL(allLines(*bufPtr), "foo x\ny");
}

BOOST_AUTO_TEST_CASE(replaceAll_smart_case_insensitive_lowercase_pattern) {
  auto bufPtr = makeBuffer({"Hello HELLO hello"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "hello", "X");
  BOOST_CHECK_EQUAL(n, 3u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "X X X");
}

BOOST_AUTO_TEST_CASE(replaceAll_smart_case_sensitive_when_pattern_has_caps) {
  auto bufPtr = makeBuffer({"Hello HELLO hello"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "Hello", "X");
  BOOST_CHECK_EQUAL(n, 1u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "X HELLO hello");
}

BOOST_AUTO_TEST_CASE(replaceAll_empty_pattern_no_op) {
  auto bufPtr = makeBuffer({"foo"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "", "bar");
  BOOST_CHECK_EQUAL(n, 0u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "foo");
}
