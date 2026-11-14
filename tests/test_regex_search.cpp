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

// Like replaceAllForward, the regex replace-all edits through Buffer and
// must record its own version so later deltas build on the replaced text.
BOOST_AUTO_TEST_CASE(replace_all_records_one_version) {
  TempFile f("a\na\na\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('x');
  std::string err;
  auto re = compileRegex("a", err);
  BOOST_REQUIRE(re.has_value());
  auto before = buf.history().size();
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf, {0, 0}, *re, "b"), 3u);
  BOOST_CHECK_EQUAL(buf.history().size(), before + 1);
  view.clampToBuffer();
  view.setPoint(2, 0);
  view.insert('y');
  auto latest = buf.history().linesAt(buf.history().size() - 1);
  BOOST_REQUIRE_EQUAL(latest.size(), buf.lineCount());
  for (std::size_t i = 0; i < latest.size(); ++i)
    BOOST_CHECK_EQUAL(*latest[i], buf.lineContent(i));
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

// --- Zero-width matches must make progress ---

namespace {
std::size_t replaceAllIn(TextBuffer& buf, char const* pattern, char const* replacement) {
  std::string err;
  auto re = compileRegex(pattern, err);
  BOOST_REQUIRE(re);
  return replaceAllRegexForward(buf, {0, 0}, *re, replacement);
}
} // namespace

BOOST_AUTO_TEST_CASE(replace_all_empty_matches_with_empty_replacement_terminates) {
  TextBuffer buf("t");
  buf.insertText("abc\nbab", BufferPoint{0, 0});
  // x* matches empty at each of the 4 + 4 positions.
  BOOST_CHECK_EQUAL(replaceAllIn(buf, "x*", ""), 8u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "abc");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "bab");
}

BOOST_AUTO_TEST_CASE(replace_all_word_boundary_inserts_once_per_boundary) {
  TextBuffer buf("t");
  buf.insertText("ab cd", BufferPoint{0, 0});
  BOOST_CHECK_EQUAL(replaceAllIn(buf, "\\b", "|"), 4u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "|ab| |cd|");
}

BOOST_AUTO_TEST_CASE(replace_all_line_anchors_terminate) {
  TextBuffer buf("t");
  buf.insertText("a\nb\n", BufferPoint{0, 0});
  BOOST_CHECK_EQUAL(replaceAllIn(buf, "^", "> "), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "> a");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "> b");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "> ");
  TextBuffer buf2("t");
  buf2.insertText("a\nb", BufferPoint{0, 0});
  BOOST_CHECK_EQUAL(replaceAllIn(buf2, "$", ";"), 2u);
  BOOST_CHECK_EQUAL(buf2.lineContent(0), "a;");
  BOOST_CHECK_EQUAL(buf2.lineContent(1), "b;");
}

BOOST_AUTO_TEST_CASE(replace_all_empty_match_steps_whole_code_points) {
  TextBuffer buf("t");
  buf.insertText("\xc3\xa9\xe4\xb8\xad", BufferPoint{0, 0}); // "é中"
  BOOST_CHECK_EQUAL(replaceAllIn(buf, "x*", "."), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), ".\xc3\xa9.\xe4\xb8\xad.");
}

// Matching used to resume in the already-replaced text, so removing the
// "a" at a line start let `^a` match the next one, and so on.
BOOST_AUTO_TEST_CASE(replace_all_matches_the_original_line_only) {
  TextBuffer buf("t");
  buf.insertText("aaxa\naa", BufferPoint{0, 0});
  std::string err;
  auto re = compileRegex("^a", err);
  BOOST_REQUIRE(re);
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf, {0, 0}, *re, ""), 2u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "axa");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "a");

  // A start offset leaves the text before it alone and still knows it is
  // not at the line start.
  TextBuffer buf2("t");
  buf2.insertText("abab", BufferPoint{0, 0});
  auto ab = compileRegex("^ab|b", err);
  BOOST_REQUIRE(ab);
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf2, {0, 2}, *ab, "X"), 1u);
  BOOST_CHECK_EQUAL(buf2.lineContent(0), "abaX");
}

BOOST_AUTO_TEST_CASE(replace_all_with_newline_replacement_continues_after_it) {
  TextBuffer buf("t");
  buf.insertText("a,b\nc,d", BufferPoint{0, 0});
  std::string err;
  auto re = compileRegex(",", err);
  BOOST_REQUIRE(re);
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf, {0, 0}, *re, "\n"), 2u);
  BOOST_REQUIRE_EQUAL(buf.lineCount(), 4u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "a");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "b");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "c");
  BOOST_CHECK_EQUAL(buf.lineContent(3), "d");
}

// One edit per line instead of one per match: 200,000 matches on a single
// line took minutes when each one shifted the rest of the line.
BOOST_AUTO_TEST_CASE(replace_all_is_linear_in_matches_per_line) {
  TextBuffer buf("t");
  std::string line;
  for (int i = 0; i < 200000; ++i)
    line += "ab ";
  buf.insertText(line, BufferPoint{0, 0});
  std::string err;
  auto re = compileRegex("a", err);
  BOOST_REQUIRE(re);
  BOOST_CHECK_EQUAL(replaceAllRegexForward(buf, {0, 0}, *re, "xyz"), 200000u);
  BOOST_CHECK_EQUAL(buf.lineContent(0).size(), line.size() + 400000u);
}

BOOST_AUTO_TEST_CASE(step_past_empty_match_moves_forward) {
  TextBuffer buf("t");
  buf.insertText("\xc3\xa9x\nz", BufferPoint{0, 0});
  BOOST_CHECK(stepPastEmptyMatch(buf, {0, 0}) == (BufferPoint{0, 2}));
  BOOST_CHECK(stepPastEmptyMatch(buf, {0, 2}) == (BufferPoint{0, 3}));
  BOOST_CHECK(stepPastEmptyMatch(buf, {0, 3}) == (BufferPoint{1, 0}));
  BOOST_CHECK(stepPastEmptyMatch(buf, {1, 0}) == (BufferPoint{1, 1}));
  BOOST_CHECK(!stepPastEmptyMatch(buf, {1, 1}).has_value());
}
