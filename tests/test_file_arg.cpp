#define BOOST_TEST_MODULE FileArg
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "file_arg.hpp"
#include "tests/test_helpers.hpp"

#include <fstream>

BOOST_AUTO_TEST_CASE(plain_path_returns_no_line) {
  auto [p, line] = parseFileArg("foo.cpp");
  BOOST_CHECK_EQUAL(p, "foo.cpp");
  BOOST_CHECK(!line);
}

BOOST_AUTO_TEST_CASE(path_with_line_splits) {
  auto [p, line] = parseFileArg("foo.cpp:42");
  BOOST_CHECK_EQUAL(p, "foo.cpp");
  BOOST_REQUIRE(line);
  BOOST_CHECK_EQUAL(*line, 42u);
}

BOOST_AUTO_TEST_CASE(absolute_path_with_line) {
  auto [p, line] = parseFileArg("/tmp/bar:7");
  BOOST_CHECK_EQUAL(p, "/tmp/bar");
  BOOST_REQUIRE(line);
  BOOST_CHECK_EQUAL(*line, 7u);
}

BOOST_AUTO_TEST_CASE(splits_at_last_colon_only) {
  auto [p, line] = parseFileArg("foo:bar:42");
  BOOST_CHECK_EQUAL(p, "foo:bar");
  BOOST_REQUIRE(line);
  BOOST_CHECK_EQUAL(*line, 42u);
}

BOOST_AUTO_TEST_CASE(trailing_colon_no_digits_keeps_whole_arg) {
  auto [p, line] = parseFileArg("foo.cpp:");
  BOOST_CHECK_EQUAL(p, "foo.cpp:");
  BOOST_CHECK(!line);
}

BOOST_AUTO_TEST_CASE(non_digit_suffix_rejects_split) {
  auto [p, line] = parseFileArg("foo:bar");
  BOOST_CHECK_EQUAL(p, "foo:bar");
  BOOST_CHECK(!line);
}

BOOST_AUTO_TEST_CASE(zero_line_treated_as_no_line) {
  auto [p, line] = parseFileArg("foo:0");
  BOOST_CHECK_EQUAL(p, "foo:0");
  BOOST_CHECK(!line);
}

BOOST_AUTO_TEST_CASE(existing_file_with_colon_in_name_kept_intact) {
  // Real file whose name contains ':' should not be split.
  TempFile tmp("data", ":99");
  auto [p, line] = parseFileArg(tmp.path().string());
  BOOST_CHECK_EQUAL(p, tmp.path().string());
  BOOST_CHECK(!line);
}

BOOST_AUTO_TEST_CASE(empty_arg_returns_empty) {
  auto [p, line] = parseFileArg("");
  BOOST_CHECK_EQUAL(p, "");
  BOOST_CHECK(!line);
}
