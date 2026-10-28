#define BOOST_TEST_MODULE Cli
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "cli.hpp"

static std::vector<std::string> filesOf(std::expected<cli::Command, std::string> const& c) {
  BOOST_REQUIRE(c.has_value());
  BOOST_REQUIRE(std::holds_alternative<cli::OpenFiles>(*c));
  return std::get<cli::OpenFiles>(*c).args;
}

BOOST_AUTO_TEST_CASE(no_arguments_opens_nothing) { BOOST_CHECK(filesOf(cli::parse({})).empty()); }

BOOST_AUTO_TEST_CASE(plain_arguments_are_files) {
  auto files = filesOf(cli::parse({"a.cpp", "src/b.hpp:42", "dir/"}));
  BOOST_REQUIRE_EQUAL(files.size(), 3u);
  BOOST_CHECK_EQUAL(files[1], "src/b.hpp:42");
}

BOOST_AUTO_TEST_CASE(help_short_and_long) {
  for (std::string_view opt : {"-h", "--help"}) {
    auto c = cli::parse({"a.cpp", opt});
    BOOST_REQUIRE(c.has_value());
    BOOST_CHECK(std::holds_alternative<cli::ShowHelp>(*c));
  }
}

BOOST_AUTO_TEST_CASE(version) {
  auto c = cli::parse({"--version"});
  BOOST_REQUIRE(c.has_value());
  BOOST_CHECK(std::holds_alternative<cli::ShowVersion>(*c));
}

BOOST_AUTO_TEST_CASE(first_of_help_and_version_wins) {
  auto c = cli::parse({"--version", "--help"});
  BOOST_REQUIRE(c.has_value());
  BOOST_CHECK(std::holds_alternative<cli::ShowVersion>(*c));
}

BOOST_AUTO_TEST_CASE(unknown_option_is_an_error) {
  auto c = cli::parse({"a.cpp", "-x"});
  BOOST_REQUIRE(!c.has_value());
  BOOST_CHECK(c.error().find("'-x'") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(double_dash_ends_options) {
  auto files = filesOf(cli::parse({"--", "--help", "-x"}));
  BOOST_REQUIRE_EQUAL(files.size(), 2u);
  BOOST_CHECK_EQUAL(files[0], "--help");
}

BOOST_AUTO_TEST_CASE(lone_dash_is_a_file) {
  auto files = filesOf(cli::parse({"-"}));
  BOOST_REQUIRE_EQUAL(files.size(), 1u);
  BOOST_CHECK_EQUAL(files[0], "-");
}

BOOST_AUTO_TEST_CASE(usage_lists_options) {
  auto text = cli::usage("cx");
  BOOST_CHECK(text.starts_with("Usage: cx "));
  BOOST_CHECK(text.find("--help") != std::string::npos);
  BOOST_CHECK(text.find("--version") != std::string::npos);
}
