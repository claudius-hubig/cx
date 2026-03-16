#define BOOST_TEST_MODULE HelpBuffer
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/help_buffer.hpp"

BOOST_AUTO_TEST_CASE(is_readonly) {
    HelpBuffer buf;
    BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(has_nonempty_content) {
    HelpBuffer buf;
    // The buffer has content (hardcoded help text) — moving to end
    // should place point past position 0.
    buf.moveEnd();
    BOOST_CHECK(buf.point() > 0u);
}

BOOST_AUTO_TEST_CASE(insert_is_noop) {
    HelpBuffer buf;
    std::size_t before = buf.point();
    buf.insert('x');
    BOOST_CHECK_EQUAL(buf.point(), before);
    BOOST_CHECK(!buf.modified());
}

BOOST_AUTO_TEST_CASE(screenRow_initially_zero) {
    HelpBuffer buf;
    // Point starts at 0, viewFrom at 0 — screen row should be 0.
    BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{80}), 0u);
}
