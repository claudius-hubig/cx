#define BOOST_TEST_MODULE GrepBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/grep_buffer.hpp"

#include <string>

namespace {

// The two backends differ in what follows the line number: git grep -z
// writes a NUL, grep -Z writes a colon. Both are exercised below.
std::string gitRecord(std::string const& path, std::string const& line, std::string const& text) {
  return path + '\0' + line + '\0' + text + '\n';
}

std::string grepRecord(std::string const& path, std::string const& line, std::string const& text) {
  return path + '\0' + line + ':' + text + '\n';
}

} // namespace

BOOST_AUTO_TEST_CASE(parses_git_grep_format) {
  auto out = gitRecord("src/a.cpp", "10", "  int x = 1;");
  auto hits = parseGrepOutput(out, "/repo");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].displayPath, "src/a.cpp");
  BOOST_CHECK_EQUAL(hits[0].line, 10u);
  BOOST_CHECK_EQUAL(hits[0].path.string(), "/repo/src/a.cpp");
  // Leading whitespace is trimmed so deep indentation stays readable.
  BOOST_CHECK_EQUAL(hits[0].text, "int x = 1;");
}

BOOST_AUTO_TEST_CASE(parses_grep_format) {
  auto out = grepRecord("./src/b.cpp", "3", "match");
  auto hits = parseGrepOutput(out, "/root");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].line, 3u);
  BOOST_CHECK_EQUAL(hits[0].text, "match");
}

BOOST_AUTO_TEST_CASE(parses_multiple_records) {
  auto out = gitRecord("a.cpp", "1", "one") + gitRecord("b.cpp", "22", "two") +
             gitRecord("c.cpp", "333", "three");
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 3u);
  BOOST_CHECK_EQUAL(hits[0].line, 1u);
  BOOST_CHECK_EQUAL(hits[1].line, 22u);
  BOOST_CHECK_EQUAL(hits[2].line, 333u);
  BOOST_CHECK_EQUAL(hits[2].displayPath, "c.cpp");
}

BOOST_AUTO_TEST_CASE(colon_in_path_is_not_a_separator) {
  // The NUL after the filename is what makes this unambiguous — a colon
  // scan would split inside the filename.
  auto out = gitRecord("odd:name.cpp", "7", "hit");
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].displayPath, "odd:name.cpp");
  BOOST_CHECK_EQUAL(hits[0].line, 7u);
}

BOOST_AUTO_TEST_CASE(colon_in_matched_text_is_preserved) {
  auto out = grepRecord("a.cpp", "5", "ratio 3:4 here");
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].text, "ratio 3:4 here");
}

BOOST_AUTO_TEST_CASE(absolute_path_is_not_rerooted) {
  auto out = gitRecord("/abs/x.cpp", "2", "t");
  auto hits = parseGrepOutput(out, "/repo");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].path.string(), "/abs/x.cpp");
}

BOOST_AUTO_TEST_CASE(malformed_records_are_skipped) {
  // Non-numeric line, empty line number, zero line number, trailing output
  // without a NUL — each dropped without taking the surrounding valid
  // records with it.
  auto out = std::string("a.cpp\0abc:t\n", 12) + std::string("b.cpp\0:t\n", 9) +
             gitRecord("c.cpp", "0", "zero") + gitRecord("good.cpp", "9", "kept") + "no-nul-here\n";
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].displayPath, "good.cpp");
}

BOOST_AUTO_TEST_CASE(newline_in_path_stays_part_of_path) {
  // Records used to be split on '\n' first, turning "sub/nl\nname.txt"
  // into a hit in "name.txt".
  auto out = gitRecord("a.cpp", "1", "one") + gitRecord("sub/nl\nname.txt", "2", "two") +
             grepRecord("x\ny", "3", "three");
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 3u);
  BOOST_CHECK_EQUAL(hits[0].displayPath, "a.cpp");
  BOOST_CHECK_EQUAL(hits[1].displayPath, "sub/nl\nname.txt");
  BOOST_CHECK_EQUAL(hits[1].line, 2u);
  BOOST_CHECK_EQUAL(hits[1].text, "two");
  BOOST_CHECK_EQUAL(hits[2].displayPath, "x\ny");
  BOOST_CHECK_EQUAL(hits[2].text, "three");
}

BOOST_AUTO_TEST_CASE(empty_output_yields_no_hits) {
  BOOST_CHECK(parseGrepOutput("", "/r").empty());
}

BOOST_AUTO_TEST_CASE(trailing_record_without_newline_is_parsed) {
  std::string out = "a.cpp";
  out += '\0';
  out += "4";
  out += '\0';
  out += "tail";
  auto hits = parseGrepOutput(out, "/r");
  BOOST_REQUIRE_EQUAL(hits.size(), 1u);
  BOOST_CHECK_EQUAL(hits[0].text, "tail");
}

BOOST_AUTO_TEST_CASE(buffer_lists_header_then_results) {
  GrepBuffer buf;
  auto hits =
      parseGrepOutput(gitRecord("a.cpp", "1", "one") + gitRecord("b.cpp", "2", "two"), "/r");
  buf.refresh("pat", "/r", hits);
  BOOST_CHECK_EQUAL(buf.lineCount(), 3u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "2 matches for \"pat\" in /r");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "a.cpp:1: one");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "b.cpp:2: two");
}

BOOST_AUTO_TEST_CASE(header_is_singular_for_one_match) {
  GrepBuffer buf;
  buf.refresh("pat", "/r", parseGrepOutput(gitRecord("a.cpp", "1", "one"), "/r"));
  BOOST_CHECK_EQUAL(buf.lineContent(0), "1 match for \"pat\" in /r");
}

BOOST_AUTO_TEST_CASE(no_matches_shows_header_only) {
  GrepBuffer buf;
  auto hint = buf.refresh("nope", "/r", {});
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "No matches for \"nope\" in /r");
  // Nowhere to go but the header.
  BOOST_CHECK_EQUAL(hint.point.line, 0u);
  BOOST_CHECK(!buf.selectedHit({0, 0}));
}

BOOST_AUTO_TEST_CASE(cursor_starts_on_first_result) {
  GrepBuffer buf;
  auto hint = buf.refresh("pat", "/r", parseGrepOutput(gitRecord("a.cpp", "1", "one"), "/r"));
  BOOST_CHECK_EQUAL(hint.point.line, buf.headerLines());
}

BOOST_AUTO_TEST_CASE(selected_hit_maps_cursor_line_past_header) {
  GrepBuffer buf;
  auto hits =
      parseGrepOutput(gitRecord("a.cpp", "11", "one") + gitRecord("b.cpp", "22", "two"), "/repo");
  buf.refresh("pat", "/repo", hits);

  BOOST_CHECK(!buf.selectedHit({0, 0}));

  auto first = buf.selectedHit({1, 0});
  BOOST_REQUIRE(first.has_value());
  BOOST_CHECK_EQUAL(first->line, 11u);
  BOOST_CHECK_EQUAL(first->path.string(), "/repo/a.cpp");

  auto second = buf.selectedHit({2, 0});
  BOOST_REQUIRE(second.has_value());
  BOOST_CHECK_EQUAL(second->line, 22u);

  // Past the last result (the cursor can sit there on an empty buffer).
  BOOST_CHECK(!buf.selectedHit({3, 0}));
}

BOOST_AUTO_TEST_CASE(refresh_replaces_previous_results) {
  GrepBuffer buf;
  buf.refresh("first", "/r", parseGrepOutput(gitRecord("a.cpp", "1", "one"), "/r"));
  BOOST_CHECK_EQUAL(buf.hits().size(), 1u);
  buf.refresh("second", "/r", {});
  BOOST_CHECK(buf.hits().empty());
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
}

BOOST_AUTO_TEST_CASE(buffer_is_readonly) {
  GrepBuffer buf;
  BOOST_CHECK(buf.readonly());
}
