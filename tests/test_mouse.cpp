#define BOOST_TEST_MODULE Mouse
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

BOOST_AUTO_TEST_CASE(middle_button_decodes) {
  // Button index 1 = middle.
  auto e = parseSgrMouse("1;7;2M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isMiddleButton());
  BOOST_CHECK(!e->isLeftButton());
  BOOST_CHECK(!e->isWheel());
  BOOST_CHECK(e->isPress());
}

BOOST_AUTO_TEST_CASE(left_button_is_not_middle) {
  auto e = parseSgrMouse("0;1;1M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isLeftButton());
  BOOST_CHECK(!e->isMiddleButton());
}

BOOST_AUTO_TEST_CASE(wheel_is_neither_left_nor_middle) {
  auto e = parseSgrMouse("64;3;3M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(!e->isLeftButton());
  BOOST_CHECK(!e->isMiddleButton());
}

BOOST_AUTO_TEST_CASE(left_drag_motion_is_left_and_motion_not_release) {
  // Left held + motion = button 0 | motion bit (32), 'M' terminator.
  auto e = parseSgrMouse("32;4;9M");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isLeftButton());
  BOOST_CHECK(e->isMotion());
  BOOST_CHECK(!e->isRelease());
}

BOOST_AUTO_TEST_CASE(left_release_is_left_and_release_not_motion) {
  // Left release = button 0, 'm' terminator.
  auto e = parseSgrMouse("0;4;9m");
  BOOST_REQUIRE(e);
  BOOST_CHECK(e->isLeftButton());
  BOOST_CHECK(e->isRelease());
  BOOST_CHECK(!e->isMotion());
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

// --- collectSgrMouseBody ---
//
// The collector pulls bytes from an arbitrary source until it sees the
// SGR terminator. It must cap the buffer at kMaxMouseBody so a hostile
// stream that never emits a terminator can't grow the input
// indefinitely.

namespace {
struct ByteQueue {
  std::string data;
  std::size_t pos = 0;
  bool operator()(char& out) {
    if (pos >= data.size())
      return false;
    out = data[pos++];
    return true;
  }
};
} // namespace

BOOST_AUTO_TEST_CASE(collect_stops_at_uppercase_terminator) {
  ByteQueue q{"0;10;5Mtail"};
  auto body = collectSgrMouseBody(std::ref(q));
  BOOST_CHECK_EQUAL(body, "0;10;5M");
  // The trailing "tail" stays in the queue for the next consumer.
  BOOST_CHECK_EQUAL(q.pos, 7u);
}

BOOST_AUTO_TEST_CASE(collect_stops_at_lowercase_terminator) {
  ByteQueue q{"3;1;1m"};
  auto body = collectSgrMouseBody(std::ref(q));
  BOOST_CHECK_EQUAL(body, "3;1;1m");
}

BOOST_AUTO_TEST_CASE(collect_stops_at_eof_without_terminator) {
  ByteQueue q{"0;10;5"};
  auto body = collectSgrMouseBody(std::ref(q));
  BOOST_CHECK_EQUAL(body, "0;10;5");
  BOOST_CHECK(!parseSgrMouse(body));
}

BOOST_AUTO_TEST_CASE(collect_caps_at_kMaxMouseBody) {
  // A stream of digits with no terminator must not be allowed to grow
  // the body without bound. Provide many more than kMaxMouseBody bytes
  // and verify the collector stops at the cap.
  std::string source(4096, '5');
  ByteQueue q{source};
  auto body = collectSgrMouseBody(std::ref(q));
  BOOST_CHECK_EQUAL(body.size(), kMaxMouseBody);
  // Body has no terminator — the parser must reject it cleanly.
  BOOST_CHECK(!parseSgrMouse(body));
}

BOOST_AUTO_TEST_CASE(collect_caps_even_when_terminator_after_limit) {
  // Terminator exists but past the cap: collector cuts off before
  // reaching it, the parser sees no terminator, no mouse event is
  // generated, and the rest of the stream is left for downstream
  // consumers (so we don't get stuck draining indefinitely).
  std::string source(kMaxMouseBody + 10, '5');
  source.back() = 'M';
  ByteQueue q{source};
  auto body = collectSgrMouseBody(std::ref(q));
  BOOST_CHECK_EQUAL(body.size(), kMaxMouseBody);
  BOOST_CHECK(body.back() != 'M');
  BOOST_CHECK(!parseSgrMouse(body));
}
