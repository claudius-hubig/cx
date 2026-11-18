#define BOOST_TEST_MODULE Regex
#include <boost/test/unit_test.hpp>

#include "regex.hpp"

#include <chrono>
#include <string>
#include <string_view>

namespace {

Regex compiled(std::string_view pattern) {
  auto re = Regex::compile(pattern);
  BOOST_REQUIRE_MESSAGE(re.has_value(), std::string(pattern) + ": " + (re ? "" : re.error()));
  return std::move(*re);
}

/** @brief The first match of @p pattern in @p line, as its text, or
 *         "<none>". */
std::string first(std::string_view pattern, std::string_view line, std::size_t from = 0) {
  auto m = compiled(pattern).search(line, from);
  if (!m)
    return "<none>";
  return std::string(line.substr(m->begin(), m->end() - m->begin()));
}

/** @brief Group @p g of the first match, "<unset>" if it did not take part. */
std::string group(std::string_view pattern, std::string_view line, std::size_t g) {
  auto m = compiled(pattern).search(line, 0);
  BOOST_REQUIRE(m);
  auto s = m->groups[g];
  if (!s.matched())
    return "<unset>";
  return std::string(line.substr(s.begin, s.end - s.begin));
}

std::string error(std::string_view pattern) {
  auto re = Regex::compile(pattern);
  return re ? "<compiled>" : re.error();
}

} // namespace

// --- Syntax ---

BOOST_AUTO_TEST_CASE(literals_and_any) {
  BOOST_CHECK_EQUAL(first("bc", "abcd"), "bc");
  BOOST_CHECK_EQUAL(first("b.d", "abcd"), "bcd");
  BOOST_CHECK_EQUAL(first("x", "abcd"), "<none>");
  BOOST_CHECK_EQUAL(first("a]}", "a]}"), "a]}"); // ] and } close nothing: literal
}

BOOST_AUTO_TEST_CASE(escapes) {
  BOOST_CHECK_EQUAL(first("\\.\\*\\\\\\/", "x.*\\/y"), ".*\\/");
  BOOST_CHECK_EQUAL(first("a\\tb", "a\tb"), "a\tb");
  BOOST_CHECK_EQUAL(first("\\x41\\u0042\\u{43}", "ABC"), "ABC");
  BOOST_CHECK_EQUAL(first("\\u{1F600}", "x\xf0\x9f\x98\x80y"), "\xf0\x9f\x98\x80");
  BOOST_CHECK_EQUAL(first("\\d+", "ab 123 c"), "123");
  BOOST_CHECK_EQUAL(first("\\D+", "12ab34"), "ab");
  BOOST_CHECK_EQUAL(first("\\w+", "  foo_1-bar"), "foo_1");
  BOOST_CHECK_EQUAL(first("\\W+", "foo -+ bar"), " -+ ");
  BOOST_CHECK_EQUAL(first("\\s+", "a \t\v\f\rb"), " \t\v\f\r");
  BOOST_CHECK_EQUAL(first("\\S+", "  ab  "), "ab");
}

BOOST_AUTO_TEST_CASE(character_classes) {
  BOOST_CHECK_EQUAL(first("[abc]+", "xxbcaz"), "bca");
  BOOST_CHECK_EQUAL(first("[^abc]+", "abxyzc"), "xyz");
  BOOST_CHECK_EQUAL(first("[a-c0-2]+", "zz1c2bq"), "1c2b");
  BOOST_CHECK_EQUAL(first("[-a]+", "x-a-"), "-a-");
  BOOST_CHECK_EQUAL(first("[a-]+", "x-a-"), "-a-");
  BOOST_CHECK_EQUAL(first("[]a]+", "x]a]"), "]a]");
  BOOST_CHECK_EQUAL(first("[\\]\\\\]+", "x]\\]"), "]\\]");
  BOOST_CHECK_EQUAL(first("[a^]+", "x^a"), "^a");
  BOOST_CHECK_EQUAL(first("[\\d_]+", "ab1_2c"), "1_2");
  BOOST_CHECK_EQUAL(first("[\\D]+", "12ab3"), "ab");
  BOOST_CHECK_EQUAL(first("[\\s,]+", "a, b"), ", ");
  BOOST_CHECK_EQUAL(first("[[]+", "a[[b"), "[[");
}

BOOST_AUTO_TEST_CASE(posix_classes) {
  BOOST_CHECK_EQUAL(first("[[:alpha:]]+", "12abC3"), "abC");
  BOOST_CHECK_EQUAL(first("[[:digit:]]+", "ab12c"), "12");
  BOOST_CHECK_EQUAL(first("[[:alnum:]]+", "--a1B--"), "a1B");
  BOOST_CHECK_EQUAL(first("[[:upper:]]+", "abCDe"), "CD");
  BOOST_CHECK_EQUAL(first("[[:lower:]]+", "ABcdE"), "cd");
  BOOST_CHECK_EQUAL(first("[[:space:]]+", "a \tb"), " \t");
  BOOST_CHECK_EQUAL(first("[[:punct:]]+", "ab!?,c"), "!?,");
  BOOST_CHECK_EQUAL(first("[[:xdigit:]]+", "xx0aFgg"), "0aF");
  BOOST_CHECK_EQUAL(first("[[:word:]]+", "--a_1--"), "a_1");
  BOOST_CHECK_EQUAL(first("[^[:digit:]x]+", "12ab3x"), "ab");
}

BOOST_AUTO_TEST_CASE(anchors_and_word_boundaries) {
  BOOST_CHECK_EQUAL(first("^a", "ab"), "a");
  BOOST_CHECK_EQUAL(first("^b", "ab"), "<none>");
  BOOST_CHECK_EQUAL(first("b$", "abb"), "b");
  BOOST_CHECK_EQUAL(first("\\bfoo\\b", "foobar foo"), "foo");
  BOOST_CHECK_EQUAL(compiled("\\bfoo\\b").search("foobar foo", 0)->begin(), 7u);
  BOOST_CHECK_EQUAL(compiled("\\Boo").search("oo foo", 0)->begin(), 4u);
  // The text before `from` is context.
  BOOST_CHECK_EQUAL(first("^a", "aa", 1), "<none>");
  BOOST_CHECK_EQUAL(first("\\ba", "ba a", 1), "a");
  BOOST_CHECK_EQUAL(compiled("\\ba").search("ba a", 1)->begin(), 3u);
}

BOOST_AUTO_TEST_CASE(groups_and_alternation) {
  BOOST_CHECK_EQUAL(first("cat|dog", "hotdog cat"), "dog");
  BOOST_CHECK_EQUAL(first("a|", "b"), "");
  BOOST_CHECK_EQUAL(group("(\\w+)=(\\w+)", "x foo=bar", 1), "foo");
  BOOST_CHECK_EQUAL(group("(\\w+)=(\\w+)", "x foo=bar", 2), "bar");
  BOOST_CHECK_EQUAL(group("(?:ab)+(c)", "ababc", 1), "c");
  BOOST_CHECK_EQUAL(group("(a)|(b)", "b", 1), "<unset>");
  BOOST_CHECK_EQUAL(group("(a)|(b)", "b", 2), "b");
  BOOST_CHECK_EQUAL(group("((a)(b))", "ab", 3), "b");
  BOOST_CHECK_EQUAL(compiled("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)").groupCount(), 11u);
  // Groups above 9 are matched but not reported.
  BOOST_CHECK_EQUAL(group("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)", "abcdefghij", 9), "i");
}

BOOST_AUTO_TEST_CASE(quantifiers) {
  BOOST_CHECK_EQUAL(first("ab*", "abbbc"), "abbb");
  BOOST_CHECK_EQUAL(first("ab*", "ac"), "a");
  BOOST_CHECK_EQUAL(first("ab+", "ac abb"), "abb");
  BOOST_CHECK_EQUAL(first("ab?c", "ac abc"), "ac");
  BOOST_CHECK_EQUAL(first("a{3}", "aaaaa"), "aaa");
  BOOST_CHECK_EQUAL(first("a{2,}", "a aaaa"), "aaaa");
  BOOST_CHECK_EQUAL(first("a{2,3}", "aaaa"), "aaa");
  BOOST_CHECK_EQUAL(first("a{0}b", "ab"), "b");
  BOOST_CHECK_EQUAL(first("(?:ab){2}", "ababab"), "abab");
}

BOOST_AUTO_TEST_CASE(lazy_quantifiers_take_as_few_as_possible) {
  BOOST_CHECK_EQUAL(first("<.*>", "<a><b>"), "<a><b>");
  BOOST_CHECK_EQUAL(first("<.*?>", "<a><b>"), "<a>");
  BOOST_CHECK_EQUAL(first("a+?", "aaa"), "a");
  BOOST_CHECK_EQUAL(first("a??b", "ab"), "ab"); // the whole pattern must still match
  BOOST_CHECK_EQUAL(first("a??", "a"), "");
  BOOST_CHECK_EQUAL(first("a{2,4}?", "aaaa"), "aa");
  BOOST_CHECK_EQUAL(first("a{2,}?", "aaaa"), "aa");
}

// --- Semantics ---

BOOST_AUTO_TEST_CASE(leftmost_then_first_by_priority) {
  BOOST_CHECK_EQUAL(first("a|ab", "ab"), "a"); // first alternative, not the longest
  BOOST_CHECK_EQUAL(first("ab|a", "ab"), "ab");
  BOOST_CHECK_EQUAL(first("b|ab", "ab"), "ab"); // leftmost start wins over alternative order
  BOOST_CHECK_EQUAL(first("(a*)(a*)", "aaa"), "aaa");
  BOOST_CHECK_EQUAL(group("(a*)(a*)", "aaa", 1), "aaa");
  BOOST_CHECK_EQUAL(group("(a*?)(a*)", "aaa", 1), "");
}

BOOST_AUTO_TEST_CASE(captures_report_the_last_iteration) {
  BOOST_CHECK_EQUAL(group("(?:(a)|b)+", "ab", 1), "a");
  BOOST_CHECK_EQUAL(group("(\\w)+", "abc", 1), "c");
}

BOOST_AUTO_TEST_CASE(empty_loops_terminate) {
  BOOST_CHECK_EQUAL(first("(a*)*b", "aab"), "aab");
  BOOST_CHECK_EQUAL(first("(a*)*", "b"), "");
  // An iteration that matches nothing ends the loop, so the empty
  // alternative loses to "a" there, as in ECMAScript.
  BOOST_CHECK_EQUAL(first("(?:|a)*", "aa"), "aa");
  BOOST_CHECK_EQUAL(first("(?:a|)*", "aa"), "aa");
}

BOOST_AUTO_TEST_CASE(matching_is_by_code_point) {
  BOOST_CHECK_EQUAL(first("a.c", "a\xc3\xa9"
                                 "c"),
                    "a\xc3\xa9"
                    "c"); // é is one unit
  BOOST_CHECK_EQUAL(first("a..c", "a\xc3\xa9"
                                  "c"),
                    "<none>");
  BOOST_CHECK_EQUAL(first("[\xc3\xa9\xe4\xb8\xad]+", "x\xe4\xb8\xad\xc3\xa9y"),
                    "\xe4\xb8\xad\xc3\xa9");
  BOOST_CHECK_EQUAL(first("[\xc3\xa0-\xc3\xbf]+", "a\xc3\xa9\xc3\xa8z"),
                    "\xc3\xa9\xc3\xa8"); // [à-ÿ]
  BOOST_CHECK_EQUAL(first("\xe4\xb8\xad+", "\xe4\xb8\xad\xe4\xb8\xad"), "\xe4\xb8\xad\xe4\xb8\xad");
  BOOST_CHECK_EQUAL(first(".", "\xf0\x9f\x98\x80"), "\xf0\x9f\x98\x80");
  // A combining mark is a code point of its own.
  BOOST_CHECK_EQUAL(first("e", "e\xcc\x81"), "e");
  // An invalid byte: matched by `.` and negated classes only.
  BOOST_CHECK_EQUAL(first("a.b", "a\xff"
                                 "b"),
                    "a\xff"
                    "b");
  BOOST_CHECK_EQUAL(first("a[^x]b", "a\xff"
                                    "b"),
                    "a\xff"
                    "b");
  BOOST_CHECK_EQUAL(first("a\\Wb", "a\xff"
                                   "b"),
                    "a\xff"
                    "b");
  BOOST_CHECK_EQUAL(first("a[\\x00-\\u{10FFFF}]b", "a\xff"
                                                   "b"),
                    "<none>");
  // Match boundaries are code-point boundaries.
  auto m = compiled("x*").search("\xc3\xa9", 0);
  BOOST_REQUIRE(m);
  BOOST_CHECK_EQUAL(m->end(), 0u);
}

// A literal every match starts with, or contains, lets the search skip
// ahead or reject a line: matches must come out the same.
BOOST_AUTO_TEST_CASE(literal_shortcuts_find_the_same_matches) {
  BOOST_CHECK_EQUAL(group("(foo)bar", "xfoofoobar", 1), "foo");
  BOOST_CHECK_EQUAL(compiled("(foo)bar").search("xfoofoobar", 0)->begin(), 4u);
  BOOST_CHECK_EQUAL(compiled("\\bfoo").search("afoo foo", 1)->begin(), 5u);
  BOOST_CHECK_EQUAL(first("^ab", "abab", 2), "<none>");
  BOOST_CHECK_EQUAL(first("x?abc", "zxabc"), "xabc");
  BOOST_CHECK_EQUAL(first("x?abc", "zabc"), "abc");
  BOOST_CHECK_EQUAL(first("a+b", "caab"), "aab");
  BOOST_CHECK_EQUAL(first("\\d+ fail", "1 fai 22 fail"), "22 fail");
  BOOST_CHECK_EQUAL(first("\\d+ fail", "1 fai 22 fai"), "<none>");
  BOOST_CHECK_EQUAL(first("(?:ab|cd)xyz", "abxy cdxyz"), "cdxyz");
  auto re = compiled("foo");
  BOOST_CHECK(re.matchAt("xfoo", 1).has_value());
  BOOST_CHECK(!re.matchAt("xfoo", 0).has_value());
}

// --- Replacement templates ---

BOOST_AUTO_TEST_CASE(expand_templates) {
  auto re = compiled("(\\w+)=(\\w+)");
  std::string line = "say foo=bar now";
  auto m = re.search(line, 0);
  BOOST_REQUIRE(m);
  auto x = [&](std::string_view t) { return Regex::expand(t, line, *m); };
  BOOST_CHECK_EQUAL(x("$2=$1"), "bar=foo");
  BOOST_CHECK_EQUAL(x("[$&]"), "[foo=bar]");
  BOOST_CHECK_EQUAL(x("[$0]"), "[foo=bar]");
  BOOST_CHECK_EQUAL(x("$$1"), "$1");
  BOOST_CHECK_EQUAL(x("<$`|$'>"), "<say | now>");
  BOOST_CHECK_EQUAL(x("$10"), "foo0"); // one digit only
  BOOST_CHECK_EQUAL(x("$3$9"), "");    // no such group: empty
  BOOST_CHECK_EQUAL(x("$x $"), "$x $");
  // The escapes a pattern uses for these characters; any other backslash
  // is literal.
  BOOST_CHECK_EQUAL(x("a\\nb\\tc\\rd\\\\e"), "a\nb\tc\rd\\e");
  BOOST_CHECK_EQUAL(x("\\q \\1 \\"), "\\q \\1 \\");
  auto opt = compiled("(a)|(b)");
  auto mb = opt.search("b", 0);
  BOOST_REQUIRE(mb);
  BOOST_CHECK_EQUAL(Regex::expand("[$1][$2]", "b", *mb), "[][b]");
}

// --- Errors ---

BOOST_AUTO_TEST_CASE(refused_features_and_syntax_errors) {
  BOOST_CHECK_EQUAL(error(""), "empty pattern");
  BOOST_CHECK_EQUAL(error("(.)\\1"), "back-references are not supported (at 4)");
  BOOST_CHECK_EQUAL(error("a(?=b)"), "lookahead and lookbehind are not supported (at 2)");
  BOOST_CHECK_EQUAL(error("(?!b)"), "lookahead and lookbehind are not supported (at 1)");
  BOOST_CHECK_EQUAL(error("(?<=b)"), "lookahead and lookbehind are not supported (at 1)");
  BOOST_CHECK_EQUAL(error("(?<n>b)"), "named groups are not supported (at 1)");
  BOOST_CHECK_EQUAL(error("(a"), "unmatched '(' (at 1)");
  BOOST_CHECK_EQUAL(error("a)"), "unmatched ')' (at 2)");
  BOOST_CHECK_EQUAL(error("[ab"), "unterminated character class (missing ']') (at 1)");
  BOOST_CHECK_EQUAL(error("*a"), "nothing to repeat (at 1)");
  BOOST_CHECK_EQUAL(error("a**"), "nothing to repeat (at 3)");
  BOOST_CHECK_EQUAL(error("^*"), "nothing to repeat (at 2)");
  BOOST_CHECK_EQUAL(error("\\b+"), "nothing to repeat (at 3)");
  BOOST_CHECK_EQUAL(error("a{2"), "'{' starts no repeat count; write \\{ for a literal '{' (at 2)");
  BOOST_CHECK_EQUAL(error("{"), "'{' starts no repeat count; write \\{ for a literal '{' (at 1)");
  BOOST_CHECK_EQUAL(error("a{1001}"), "repeat count over 1000 (at 2)");
  BOOST_CHECK_EQUAL(error("a{3,2}"), "repeat range out of order (at 2)");
  BOOST_CHECK_EQUAL(error("[z-a]"), "range out of order (at 2)");
  BOOST_CHECK_EQUAL(error("[a-\\d]"), "a range cannot end in a class escape (at 2)");
  BOOST_CHECK_EQUAL(error("[\\b]"), "\\b is not allowed in a character class (at 2)");
  BOOST_CHECK_EQUAL(error("[[:alfa:]]"), "unknown POSIX class [:alfa:] (at 2)");
  BOOST_CHECK_EQUAL(error("\\q"), "unknown escape \\q (at 1)");
  BOOST_CHECK_EQUAL(error("\\0"), "unknown escape \\0 (at 1)");
  BOOST_CHECK_EQUAL(error("a\\"), "pattern ends with a backslash (at 2)");
  BOOST_CHECK_EQUAL(error("\\x4"), "\\x needs two hex digits (at 1)");
  BOOST_CHECK_EQUAL(error("\\uD800"), "\\u names no character (at 1)");
  BOOST_CHECK_EQUAL(error("\\u{110000}"), "\\u names no character (at 1)");
  BOOST_CHECK_EQUAL(error("(?:a{1000}){1000}"), "pattern too large (at 1)");
  BOOST_CHECK_EQUAL(error(std::string(101, '(') + std::string(101, ')')),
                    "groups nested too deeply (at 101)");
  BOOST_CHECK_EQUAL(error("a\xff"), "invalid UTF-8 in the pattern (at 2)");
  // Positions count characters, not bytes.
  BOOST_CHECK_EQUAL(error("\xc3\xa9\xc3\xa9)"), "unmatched ')' (at 3)");
}

// --- Time ---

// std::regex backtracked: these took from seconds to forever.
BOOST_AUTO_TEST_CASE(pathological_patterns_take_linear_time) {
  struct Case {
    char const* pattern;
    std::string line;
  };
  Case const cases[] = {
      {"(a*)*b", std::string(30, 'a')},
      {"(x+x+)+y", std::string(30, 'x')},
      {"(a|b)*c", std::string(200000, 'a')},
      {".*x", std::string(200000, 'a')},
      {"[a-z]*z", std::string(200000, 'a')},
      {"(a|aa)*b", std::string(200000, 'a')},
      {"(?:a?){1000}a{1000}", std::string(1000, 'a')},
  };
  for (auto const& c : cases) {
    auto re = compiled(c.pattern);
    auto t0 = std::chrono::steady_clock::now();
    (void)re.search(c.line, 0);
    auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    BOOST_TEST_CONTEXT(c.pattern) { BOOST_CHECK_LT(ms, 1000.0); }
  }
}

// --- Matching across lines ---

namespace {

struct Lines {
  std::vector<std::string> v;
  Regex::LineSource src() const {
    return {[this](std::size_t i) { return std::string_view(v[i]); }, v.size()};
  }
};

/** @brief The first match over @p lines as "line:off-line:off", or "<none>". */
std::string firstOver(std::string_view pattern, Lines const& lines, BufferPoint from = {0, 0}) {
  auto m = compiled(pattern).searchLines(lines.src(), from);
  if (!m)
    return "<none>";
  return std::to_string(m->begin().line) + ":" + std::to_string(m->begin().offset) + "-" +
         std::to_string(m->end().line) + ":" + std::to_string(m->end().offset);
}

} // namespace

BOOST_AUTO_TEST_CASE(which_patterns_span_lines) {
  BOOST_CHECK(compiled("a\\nb").spansLines());
  BOOST_CHECK(compiled("a\nb").spansLines()); // a pasted newline
  BOOST_CHECK(compiled("\\s").spansLines());
  BOOST_CHECK(compiled("[\\n]").spansLines());
  BOOST_CHECK(compiled("[[:space:]]").spansLines());
  BOOST_CHECK(!compiled("a.b").spansLines());
  BOOST_CHECK(!compiled("[^a]").spansLines());
  BOOST_CHECK(!compiled("\\D\\W\\S[\\D]").spansLines());
  BOOST_CHECK(!compiled("[ \\t\\r]").spansLines());
}

BOOST_AUTO_TEST_CASE(line_breaks_match_newline_escapes) {
  Lines l{{"x abc", "def y", "abc", "z"}};
  BOOST_CHECK_EQUAL(firstOver("abc\\ndef", l), "0:2-1:3");
  BOOST_CHECK_EQUAL(firstOver("abc\\ndef", l, {0, 3}), "<none>");
  BOOST_CHECK_EQUAL(firstOver("c\\s+d", l), "0:4-1:1");
  BOOST_CHECK_EQUAL(firstOver("y\\n\\w+\\nz", l), "1:4-3:1");
  BOOST_CHECK_EQUAL(firstOver("\\n", l), "0:5-1:0");
  BOOST_CHECK_EQUAL(firstOver("z\\n", l), "<none>"); // no break after the last line
  // ^ and $ at every line start and end.
  BOOST_CHECK_EQUAL(firstOver("$\\n^d", l), "0:5-1:1");
}

BOOST_AUTO_TEST_CASE(dot_and_negated_classes_stop_at_a_line_break) {
  Lines l{{"ab", "cd"}};
  BOOST_CHECK_EQUAL(firstOver("b.c", l), "<none>");
  BOOST_CHECK_EQUAL(firstOver("b[^x]c", l), "<none>");
  BOOST_CHECK_EQUAL(firstOver("b\\Dc", l), "<none>");
  BOOST_CHECK_EQUAL(firstOver("b\\Wc", l), "<none>");
  BOOST_CHECK_EQUAL(firstOver("b[\\D]c", l), "<none>");
  BOOST_CHECK_EQUAL(firstOver("b\\n?c", l), "0:1-1:1");
  // ... but they match tab and carriage return.
  BOOST_CHECK_EQUAL(first("a[^x][^x]b", "a\t\rb"), "a\t\rb");
  BOOST_CHECK_EQUAL(first("a..b", "a\t\rb"), "a\t\rb");
}

BOOST_AUTO_TEST_CASE(groups_and_replacements_across_lines) {
  Lines l{{"key =", "  value;"}};
  auto re = compiled("(\\w+) =\\s*(\\w+)");
  auto m = re.searchLines(l.src(), {0, 0});
  BOOST_REQUIRE(m);
  BOOST_CHECK(m->end() == (BufferPoint{1, 7}));
  BOOST_CHECK_EQUAL(Regex::expandLines("$2=$1", l.src(), *m), "value=key");
  BOOST_CHECK_EQUAL(Regex::expandLines("[$&]", l.src(), *m), "[key =\n  value]");
  BOOST_CHECK_EQUAL(Regex::expandLines("<$'>", l.src(), *m), "<;>");
  BOOST_CHECK(re.matchAtLines(l.src(), {0, 0}).has_value());
  BOOST_CHECK(!re.matchAtLines(l.src(), {0, 3}).has_value());
}

BOOST_AUTO_TEST_CASE(search_over_lines_can_be_cancelled) {
  Lines l;
  for (int i = 0; i < 30000; ++i)
    l.v.push_back(std::string(60, 'x'));
  l.v.push_back("end");
  ScanCancel stop = [] { return true; };
  bool cancelled = false;
  auto m = compiled("x\\nend").searchLines(l.src(), {0, 0}, &stop, &cancelled);
  BOOST_CHECK(!m);
  BOOST_CHECK(cancelled);
  ScanCancel go = [] { return false; };
  m = compiled("x\\nend").searchLines(l.src(), {0, 0}, &go, &cancelled);
  BOOST_CHECK(m);
  BOOST_CHECK(!cancelled);
}
