#define BOOST_TEST_MODULE Mouse
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "mouse.hpp"

BOOST_AUTO_TEST_CASE(left_press_decodes) {
  auto e = parseSgrMouse("0;10;5M");
  BOOST_REQUIRE(e);
  BOOST_CHECK_EQUAL(e->button, 0u);
  BOOST_CHECK_EQUAL(e->col, 10u);
  BOOST_CHECK_EQUAL(e->row, 5u);
  BOOST_CHECK(e->isPress());
  BOOST_CHECK(e->isLeftButton());
  BOOST_CHECK(!e->isWheelUp());
}

BOOST_AUTO_TEST_CASE(left_release_lowercase_terminator) {
  auto e = parseSgrMouse("0;10;5m");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isRelease());
}

BOOST_AUTO_TEST_CASE(wheel_up_decodes) {
  auto e = parseSgrMouse("64;3;3M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isWheelUp());
  BOOST_CHECK(!e->isWheelDown());
}

BOOST_AUTO_TEST_CASE(wheel_down_decodes) {
  auto e = parseSgrMouse("65;3;3M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isWheelDown());
}

BOOST_AUTO_TEST_CASE(motion_event_recognised) {
  // Button 0 + motion bit (32) = 32.
  auto e = parseSgrMouse("32;10;5M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isMotion());
  BOOST_CHECK(e->isLeftButton()); // low two bits zero
}

BOOST_AUTO_TEST_CASE(extra_bytes_after_terminator_ignored) {
  auto e = parseSgrMouse("0;1;1Mtrailing");
  BOOST_REQUIRE(e);
  BOOST_CHECK_EQUAL(e->col, 1u);
}

BOOST_AUTO_TEST_CASE(missing_terminator_fails) { BOOST_CHECK(!parseSgrMouse("0;10;5")); }

BOOST_AUTO_TEST_CASE(non_numeric_field_fails) { BOOST_CHECK(!parseSgrMouse("0;ab;5M")); }

BOOST_AUTO_TEST_CASE(missing_field_fails) { BOOST_CHECK(!parseSgrMouse("0;5M")); }

BOOST_AUTO_TEST_CASE(empty_fails) { BOOST_CHECK(!parseSgrMouse("")); }

BOOST_AUTO_TEST_CASE(trailing_semicolon_fails) { BOOST_CHECK(!parseSgrMouse("0;5;5;M")); }
