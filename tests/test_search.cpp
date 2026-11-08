#define BOOST_TEST_MODULE Search
#include <boost/test/unit_test.hpp>

#include "search.hpp"

#include <string>

// ----- FindFlags -----

BOOST_AUTO_TEST_CASE(flags_all_lowercase_is_case_insensitive) {
  FindFlags f("hello");
  BOOST_CHECK(!f.caseSensitive);
  BOOST_CHECK(!f.matchDigits);
  BOOST_CHECK(!f.matchSpecial);
}

BOOST_AUTO_TEST_CASE(flags_any_uppercase_is_case_sensitive) {
  FindFlags f("Hello");
  BOOST_CHECK(f.caseSensitive);
}

BOOST_AUTO_TEST_CASE(flags_digits_trigger_matchDigits) {
  FindFlags f("abc1");
  BOOST_CHECK(f.matchDigits);
}

BOOST_AUTO_TEST_CASE(flags_special_char_triggers_matchSpecial) {
  FindFlags f("a_b");
  BOOST_CHECK(f.matchSpecial);
}

BOOST_AUTO_TEST_CASE(flags_first_char_can_match_both_cases_when_insensitive) {
  FindFlags f("hello");
  BOOST_CHECK(f.firstCharCanMatch('h'));
  BOOST_CHECK(f.firstCharCanMatch('H'));
  BOOST_CHECK(!f.firstCharCanMatch('x'));
}

BOOST_AUTO_TEST_CASE(flags_first_char_exact_when_case_sensitive) {
  FindFlags f("Hello");
  BOOST_CHECK(f.firstCharCanMatch('H'));
  BOOST_CHECK(!f.firstCharCanMatch('h'));
}

// ----- matchAt -----

BOOST_AUTO_TEST_CASE(matchAt_exact_ascii) {
  auto r = matchAt("hello world", 0, "hello", true, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 5u);
}

BOOST_AUTO_TEST_CASE(matchAt_empty_pattern_returns_off) {
  auto r = matchAt("hello", 3, "", false, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 3u);
}

BOOST_AUTO_TEST_CASE(matchAt_nomatch_returns_nullopt) {
  auto r = matchAt("hello", 0, "xyz", false, false, false);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(matchAt_past_end_of_line) {
  auto r = matchAt("hello", 10, "hi", false, false, false);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(matchAt_case_insensitive_ascii) {
  auto r = matchAt("Hello World", 0, "hello", false, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 5u);
}

BOOST_AUTO_TEST_CASE(matchAt_case_sensitive_ascii_nomatch) {
  auto r = matchAt("Hello", 0, "hello", true, false, false);
  BOOST_CHECK(!r);
}

// First char must match directly — skipping applies only to later chars.
BOOST_AUTO_TEST_CASE(matchAt_first_char_does_not_skip) {
  // Pattern "bc" at offset 0 of "abc": first char 'b' does not match
  // 'a', so no match even though 'b' exists later.
  auto r = matchAt("abc", 0, "bc", false, false, false);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(matchAt_skips_digits_between_letters) {
  // Pattern has no digits -> matchDigits=false -> skip digits in buffer.
  // "a1b" should match pattern "ab" starting at offset 0.
  auto r = matchAt("a1b", 0, "ab", false, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 3u);
}

BOOST_AUTO_TEST_CASE(matchAt_does_not_skip_digits_when_pattern_has_digits) {
  // Pattern "ab" with matchDigits=true -> digit is not skipped, so 'a'
  // followed by '1' then 'b' does NOT match pattern "ab" at offset 0
  // because after matching 'a' the next buffer char is '1' which must
  // equal pattern's 'b' (it doesn't).
  auto r = matchAt("a1b", 0, "ab", false, true, false);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(matchAt_skips_special_between_letters) {
  // Underscore is a "special" char — skipped when pattern has none.
  auto r = matchAt("a_b", 0, "ab", false, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 3u);
}

BOOST_AUTO_TEST_CASE(matchAt_does_not_skip_special_when_pattern_has_any) {
  auto r = matchAt("a_b", 0, "ab", false, false, true);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(matchAt_skips_utf8_when_no_special_in_pattern) {
  // "a\xc3\xa9b" = "aéb" — 'é' is a 2-byte UTF-8 char. Pattern "ab"
  // with matchSpecial=false should skip 'é' entirely (utf8SeqLen=2).
  auto r = matchAt("a\xc3\xa9"
                   "b",
                   0, "ab", false, false, false);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(*r, 4u); // 'a' + 2 bytes 'é' + 'b'
}

BOOST_AUTO_TEST_CASE(matchAt_partial_match_then_fails) {
  // After matching 'a', cannot find 'z' before end of line.
  auto r = matchAt("abc", 0, "az", false, false, true);
  BOOST_CHECK(!r);
}

// ----- findFirstMatch -----

BOOST_AUTO_TEST_CASE(findFirstMatch_finds_at_nonzero_offset) {
  FindFlags f("world");
  auto r = findFirstMatch("hello world", 0, "world", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 6u);
  BOOST_CHECK_EQUAL(r->second, 11u);
}

BOOST_AUTO_TEST_CASE(findFirstMatch_respects_from_parameter) {
  FindFlags f("ab");
  // "ab...ab": searching from offset 3 should skip the first match.
  auto r = findFirstMatch("ab xab", 3, "ab", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 4u);
  BOOST_CHECK_EQUAL(r->second, 6u);
}

BOOST_AUTO_TEST_CASE(findFirstMatch_case_insensitive_default) {
  FindFlags f("hello");
  auto r = findFirstMatch("say HELLO now", 0, "hello", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 4u);
  BOOST_CHECK_EQUAL(r->second, 9u);
}

BOOST_AUTO_TEST_CASE(findFirstMatch_case_sensitive_pattern) {
  FindFlags f("HELLO");
  auto r = findFirstMatch("say hello HELLO", 0, "HELLO", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 10u);
}

BOOST_AUTO_TEST_CASE(findFirstMatch_no_match_returns_nullopt) {
  FindFlags f("zzz");
  auto r = findFirstMatch("hello", 0, "zzz", f);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(findFirstMatch_smart_skip_digits) {
  FindFlags f("ab");
  // Buffer has 'a1b' and 'ab'. Smart find should match at offset 0 via
  // skipping the '1'.
  auto r = findFirstMatch("a1b ab", 0, "ab", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 0u);
}

// ----- findLastMatch -----

BOOST_AUTO_TEST_CASE(findLastMatch_returns_last_within_range) {
  FindFlags f("ab");
  // Three matches: offsets 0, 3, 6.
  auto r = findLastMatch("ab ab ab", 0, 8, "ab", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 6u);
  BOOST_CHECK_EQUAL(r->second, 8u);
}

BOOST_AUTO_TEST_CASE(findLastMatch_respects_limit) {
  FindFlags f("ab");
  // With limit=6, only matches at offsets 0 and 3 are considered.
  auto r = findLastMatch("ab ab ab", 0, 6, "ab", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 3u);
}

BOOST_AUTO_TEST_CASE(findLastMatch_respects_from) {
  FindFlags f("ab");
  // With from=2, first match at offset 0 is skipped.
  auto r = findLastMatch("ab ab ab", 2, 8, "ab", f);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(r->first, 6u);
}

BOOST_AUTO_TEST_CASE(findLastMatch_no_match_returns_nullopt) {
  FindFlags f("zz");
  auto r = findLastMatch("hello", 0, 5, "zz", f);
  BOOST_CHECK(!r);
}

BOOST_AUTO_TEST_CASE(findLastMatch_empty_range) {
  FindFlags f("a");
  auto r = findLastMatch("aaa", 2, 2, "a", f);
  BOOST_CHECK(!r);
}
