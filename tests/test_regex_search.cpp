#define BOOST_TEST_MODULE RegexSearch
#include <boost/test/unit_test.hpp>

#include "buffer/text_buffer.hpp"
#include "regex_search.hpp"
#include "test_helpers.hpp"

BOOST_AUTO_TEST_CASE(compile_invalid_pattern_returns_nullopt) {
  std::string err;
  auto re = compileRegex("(unbalanced", err);
  BOOST_CHECK(!re.has_value());
  BOOST_CHECK(!err.empty());
}

BOOST_AUTO_TEST_CASE(compile_empty_pattern_returns_nullopt) {
  std::string err;
  auto re = compileRegex("", err);
  BOOST_CHECK(!re.has_value());
}

BOOST_AUTO_TEST_CASE(find_first_returns_match_offsets) {
  std::string err;
  auto re = compileRegex("\\d+", err);
  BOOST_REQUIRE(re.has_value());
  std::string line = "abc 123 def 456";
  auto m = findFirstRegex(line, 0, *re);
  BOOST_REQUIRE(m.has_value());
  BOOST_CHECK_EQUAL(m->first, 4u);
  BOOST_CHECK_EQUAL(m->second, 7u);
}

BOOST_AUTO_TEST_CASE(find_first_starts_from_offset) {
  std::string err;
  auto re = compileRegex("\\d+", err);
  BOOST_REQUIRE(re.has_value());
  std::string line = "abc 123 def 456";
  auto m = findFirstRegex(line, 8, *re);
  BOOST_REQUIRE(m.has_value());
  BOOST_CHECK_EQUAL(m->first, 12u);
  BOOST_CHECK_EQUAL(m->second, 15u);
}

BOOST_AUTO_TEST_CASE(find_last_in_range) {
  std::string err;
  auto re = compileRegex("\\d+", err);
  BOOST_REQUIRE(re.has_value());
  std::string line = "1 22 333 4444";
  auto m = findLastRegex(line, 0, line.size(), *re);
  BOOST_REQUIRE(m.has_value());
  BOOST_CHECK_EQUAL(m->first, 9u);
  BOOST_CHECK_EQUAL(m->second, 13u);
}

BOOST_AUTO_TEST_CASE(find_in_buffer_walks_lines) {
  TempFile f("aaa\n111\nbbb 222\n");
  TextBuffer buf(f.path().string(), f.path());
  std::string err;
  auto re = compileRegex("\\d+", err);
  BOOST_REQUIRE(re.has_value());

  auto m = findNextRegexInBuffer(buf, BufferPoint{0, 0}, *re);
  BOOST_REQUIRE(m.has_value());
  BOOST_CHECK_EQUAL(m->first.line, 1u);
  BOOST_CHECK_EQUAL(m->first.offset, 0u);

  // Continue past the first match.
  auto m2 = findNextRegexInBuffer(buf, m->second, *re);
  BOOST_REQUIRE(m2.has_value());
  BOOST_CHECK_EQUAL(m2->first.line, 2u);
  BOOST_CHECK_EQUAL(m2->first.offset, 4u);
}

BOOST_AUTO_TEST_CASE(substitute_captures_resolves_dollar_refs) {
  std::string err;
  auto re = compileRegex("(\\w+)=(\\w+)", err);
  BOOST_REQUIRE(re.has_value());
  std::string line = "foo=bar";
  auto rendered = substituteCaptures(line, 0, line.size(), *re, "$2=$1");
  BOOST_CHECK_EQUAL(rendered, "bar=foo");
}

BOOST_AUTO_TEST_CASE(replace_all_forward_terminates_on_self_matching_replacement) {
  TempFile f("aaa aaa aaa\n");
  TextBuffer buf(f.path().string(), f.path());
  std::string err;
  auto re = compileRegex("aaa", err);
  BOOST_REQUIRE(re.has_value());

  // Replacement contains "aaa" as a substring; the scan must skip the
  // inserted text rather than re-match it.
  auto count = replaceAllRegexForward(buf, BufferPoint{0, 0}, *re, "aaa-aaa");
  BOOST_CHECK_EQUAL(count, 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aaa-aaa aaa-aaa aaa-aaa");
}

BOOST_AUTO_TEST_CASE(replace_all_with_back_references) {
  TempFile f("foo=1, bar=2\n");
  TextBuffer buf(f.path().string(), f.path());
  std::string err;
  auto re = compileRegex("(\\w+)=(\\d+)", err);
  BOOST_REQUIRE(re.has_value());

  auto count = replaceAllRegexForward(buf, BufferPoint{0, 0}, *re, "$1:$2");
  BOOST_CHECK_EQUAL(count, 2u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "foo:1, bar:2");
}

// --- Mid-line context: the text before `from` is not a line start ---

BOOST_AUTO_TEST_CASE(find_first_caret_does_not_match_mid_line) {
  std::string err;
  auto re = compileRegex("^a", err);
  BOOST_REQUIRE(re);
  BOOST_CHECK(!findFirstRegex("ba", 1, *re).has_value());
  auto m = findFirstRegex("ab", 0, *re);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first, 0u);
}

BOOST_AUTO_TEST_CASE(find_first_word_boundary_sees_previous_char) {
  std::string err;
  auto re = compileRegex("\\bb", err);
  BOOST_REQUIRE(re);
  BOOST_CHECK(!findFirstRegex("ab", 1, *re).has_value());
  auto m = findFirstRegex("a b", 1, *re);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first, 2u);
}

BOOST_AUTO_TEST_CASE(find_last_caret_only_at_line_start) {
  std::string err;
  auto re = compileRegex("^a", err);
  BOOST_REQUIRE(re);
  auto m = findLastRegex("aaa", 0, 3, *re);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first, 0u);
}

BOOST_AUTO_TEST_CASE(substitute_captures_with_lookahead) {
  std::string err;
  auto re = compileRegex("(a)(?=b)", err);
  BOOST_REQUIRE(re);
  std::string line = "ab";
  auto m = findFirstRegex(line, 0, *re);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(substituteCaptures(line, m->first, m->second, *re, "[$1]"), "[a]");
}

BOOST_AUTO_TEST_CASE(substitute_captures_with_word_boundary_mid_line) {
  std::string err;
  auto re = compileRegex("\\b(\\w)", err);
  BOOST_REQUIRE(re);
  std::string line = "xy zw";
  auto m = findFirstRegex(line, 1, *re);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first, 3u);
  BOOST_CHECK_EQUAL(substituteCaptures(line, m->first, m->second, *re, "<$1>"), "<z>");
}

BOOST_AUTO_TEST_CASE(replace_all_caret_only_replaces_line_starts) {
  std::string err;
  auto re = compileRegex("^a", err);
  BOOST_REQUIRE(re);
  TextBuffer buf("t");
  buf.insertText("aaa\naba", BufferPoint{0, 0});
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf, {0, 0}, *re, "X"), 2u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "Xaa");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "Xba");
}
