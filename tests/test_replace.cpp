#define BOOST_TEST_MODULE Replace
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "replace.hpp"
#include "test_helpers.hpp"

#include <memory>
#include <string>
#include <vector>

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

// ----- findNextMatchInBuffer (exact substring) -----

BOOST_AUTO_TEST_CASE(findNext_returns_first_match_on_start_line) {
  auto bufPtr = makeBuffer({"hello world", "second line"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "world");
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.line, 0u);
  BOOST_CHECK_EQUAL(m->first.offset, 6u);
  BOOST_CHECK_EQUAL(m->second.offset, 11u);
}

BOOST_AUTO_TEST_CASE(findNext_skips_to_later_line) {
  auto bufPtr = makeBuffer({"alpha", "beta gamma", "delta"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "gamma");
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.line, 1u);
  BOOST_CHECK_EQUAL(m->first.offset, 5u);
}

BOOST_AUTO_TEST_CASE(findNext_respects_start_offset) {
  auto bufPtr = makeBuffer({"foo foo foo"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 4}, "foo");
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->first.offset, 4u);
}

BOOST_AUTO_TEST_CASE(findNext_no_wrap_returns_nullopt) {
  auto bufPtr = makeBuffer({"alpha", "beta"});
  auto m = findNextMatchInBuffer(*bufPtr, {1, 0}, "alpha");
  BOOST_CHECK(!m);
}

BOOST_AUTO_TEST_CASE(findNext_does_not_skip_digits) {
  // Replace must not honour smart-find skipping: "abc" must not match
  // "a1b2c". Otherwise the user could ask to replace "abc" and have
  // "a1b2c" silently rewritten.
  auto bufPtr = makeBuffer({"a1b2c"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "abc");
  BOOST_CHECK(!m);
}

BOOST_AUTO_TEST_CASE(findNext_is_case_sensitive) {
  // Same rationale: lowercase pattern must only hit the literal bytes.
  auto bufPtr = makeBuffer({"Hello"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "hello");
  BOOST_CHECK(!m);
}

BOOST_AUTO_TEST_CASE(findNext_empty_pattern_returns_nullopt) {
  auto bufPtr = makeBuffer({"foo"});
  auto m = findNextMatchInBuffer(*bufPtr, {0, 0}, "");
  BOOST_CHECK(!m);
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

BOOST_AUTO_TEST_CASE(replaceAll_is_case_sensitive) {
  auto bufPtr = makeBuffer({"Hello HELLO hello"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "hello", "X");
  BOOST_CHECK_EQUAL(n, 1u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "Hello HELLO X");
}

BOOST_AUTO_TEST_CASE(replaceAll_does_not_skip_intervening_chars) {
  auto bufPtr = makeBuffer({"a1b1c"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "abc", "X");
  BOOST_CHECK_EQUAL(n, 0u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "a1b1c");
}

BOOST_AUTO_TEST_CASE(replaceAll_empty_pattern_no_op) {
  auto bufPtr = makeBuffer({"foo"});
  auto n = replaceAllForward(*bufPtr, {0, 0}, "", "bar");
  BOOST_CHECK_EQUAL(n, 0u);
  BOOST_CHECK_EQUAL(bufPtr->lineContent(0), "foo");
}

// Replace-all edits through Buffer, not BufferView, so it must record its
// own version: otherwise the next single-line edit is stored as a delta
// against a snapshot without the replacements, and that version rebuilds
// to text that never existed.
BOOST_AUTO_TEST_CASE(replaceAll_records_one_version) {
  // Loaded from a file so the initial version holds the text (makeBuffer's
  // direct Buffer edits record nothing).
  TempFile f("a\na\na");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.insert('x');
  auto before = buf.history().size();
  replaceAllForward(buf, {0, 0}, "a", "b");
  BOOST_CHECK_EQUAL(buf.history().size(), before + 1);
  view.clampToBuffer();
  view.setPoint(2, 0);
  view.insert('y');
  std::vector<std::string> latest;
  for (auto const& l : buf.history().linesAt(buf.history().size() - 1))
    latest.push_back(*l);
  BOOST_CHECK_EQUAL(allLines(buf), "xb\nb\nyb");
  BOOST_CHECK(latest == (std::vector<std::string>{"xb", "b", "yb"}));
}

BOOST_AUTO_TEST_CASE(replaceAll_without_matches_records_nothing) {
  auto bufPtr = makeBuffer({"a"});
  auto before = bufPtr->history().size();
  replaceAllForward(*bufPtr, {0, 0}, "zzz", "b");
  BOOST_CHECK_EQUAL(bufPtr->history().size(), before);
}

// --- Patterns with a newline match across lines ---

BOOST_AUTO_TEST_CASE(pattern_lines_split_at_newlines) {
  BOOST_CHECK((patternLines("abc") == std::vector<std::string_view>{"abc"}));
  BOOST_CHECK((patternLines("a\nb\nc") == std::vector<std::string_view>{"a", "b", "c"}));
  BOOST_CHECK((patternLines("\n") == std::vector<std::string_view>{"", ""}));
}

BOOST_AUTO_TEST_CASE(multi_line_pattern_ends_a_line_and_starts_the_next) {
  auto buf = makeBuffer({"x abc", "def y", "abc", "mid", "def"});
  auto m = findNextMatchInBuffer(*buf, {0, 0}, "abc\ndef");
  BOOST_REQUIRE(m);
  BOOST_CHECK(m->first == (BufferPoint{0, 2}));
  BOOST_CHECK(m->second == (BufferPoint{1, 3}));
  // "abc" on line 2 is followed by "mid", not by a line starting "def".
  BOOST_CHECK(!findNextMatchInBuffer(*buf, m->second, "abc\ndef"));
  // A middle piece must be a whole line.
  auto m3 = findNextMatchInBuffer(*buf, {0, 0}, "abc\nmid\nde");
  BOOST_REQUIRE(m3);
  BOOST_CHECK(m3->first == (BufferPoint{2, 0}));
  BOOST_CHECK(m3->second == (BufferPoint{4, 2}));
  BOOST_CHECK(!findNextMatchInBuffer(*buf, {0, 0}, "abc\nmi\ndef"));
  // The start may not be before `start`.
  BOOST_CHECK(!findNextMatchInBuffer(*buf, {0, 3}, "abc\ndef\n"));
}

BOOST_AUTO_TEST_CASE(find_previous_multi_line_match) {
  auto buf = makeBuffer({"a", "b", "a", "b"});
  auto last = findPrevMatchInBuffer(*buf, {4, 0}, "a\nb");
  BOOST_REQUIRE(last);
  BOOST_CHECK(last->first == (BufferPoint{2, 0}));
  // The point at the end of that match: the one before it.
  auto prev = findPrevMatchInBuffer(*buf, last->second, "a\nb");
  BOOST_REQUIRE(prev);
  BOOST_CHECK(prev->first == (BufferPoint{0, 0}));
  BOOST_CHECK(!findPrevMatchInBuffer(*buf, prev->second, "a\nb"));
}

BOOST_AUTO_TEST_CASE(replace_all_removes_and_adds_newlines) {
  auto buf = makeBuffer({"1 abc", "def 2", "abc", "def"});
  BOOST_CHECK_EQUAL(replaceAllForward(*buf, {0, 0}, "abc\ndef", "abc def"), 2u);
  BOOST_CHECK_EQUAL(allLines(*buf), "1 abc def 2\nabc def");

  auto buf2 = makeBuffer({"xabcx"});
  BOOST_CHECK_EQUAL(replaceAllForward(*buf2, {0, 0}, "abc", "a\nb\nc"), 1u);
  BOOST_CHECK_EQUAL(allLines(*buf2), "xa\nb\ncx");

  // Every line break, joined into one line, as one history version.
  auto buf3 = makeBuffer({"a", "b", "", "c"});
  auto versions = buf3->history().size();
  BOOST_CHECK_EQUAL(replaceAllForward(*buf3, {0, 0}, "\n", " "), 3u);
  BOOST_CHECK_EQUAL(allLines(*buf3), "a b  c");
  BOOST_CHECK_EQUAL(buf3->history().size(), versions + 1);
}

BOOST_AUTO_TEST_CASE(replace_all_multi_line_never_rematches_replacements) {
  auto buf = makeBuffer({"x", "x", "x", "x"});
  BOOST_CHECK_EQUAL(replaceAllForward(*buf, {0, 0}, "x\nx", "x\nx\nx"), 2u);
  BOOST_CHECK_EQUAL(allLines(*buf), "x\nx\nx\nx\nx\nx");
}

// Lines between separate rewrites keep their storage, which earlier
// history versions share.
BOOST_AUTO_TEST_CASE(replace_all_multi_line_leaves_lines_between_alone) {
  auto buf = makeBuffer({"a", "b", "keep", "a", "b"});
  auto const* before = &buf->lineContent(2);
  BOOST_CHECK_EQUAL(replaceAllForward(*buf, {0, 0}, "a\nb", "ab"), 2u);
  BOOST_CHECK_EQUAL(allLines(*buf), "ab\nkeep\nab");
  BOOST_CHECK_EQUAL(&buf->lineContent(1), before);
}
