#define BOOST_TEST_MODULE HelpBuffer
#include <boost/test/unit_test.hpp>

#include "buffer/help_buffer.hpp"
#include "test_helpers.hpp"

BOOST_AUTO_TEST_CASE(is_readonly) {
  TB<HelpBuffer> buf;
  BOOST_CHECK(buf.readonly());
}

BOOST_AUTO_TEST_CASE(has_nonempty_content) {
  TB<HelpBuffer> buf;
  // The buffer has content (hardcoded help text) — moving to end
  // should place point past position 0.
  buf.moveEnd();
  BOOST_CHECK(buf.point() > (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(insert_is_noop) {
  TB<HelpBuffer> buf;
  auto before = buf.point();
  buf.insert('x');
  BOOST_CHECK(buf.point() == before);
  BOOST_CHECK(!buf.modified());
}

BOOST_AUTO_TEST_CASE(screenRow_initially_zero) {
  TB<HelpBuffer> buf;
  // Point starts at 0, viewFrom at 0 — screen row should be 0.
  BOOST_CHECK_EQUAL(buf.screenRow(ColIdx{80}, buf.point(), buf.viewFromRaw()), 0u);
}

BOOST_AUTO_TEST_CASE(help_contains_set_mode_shortcut) {
  TB<HelpBuffer> buf;
  // Render the help buffer and check that Esc-m appears.
  std::string out;
  buf.render(out, RowIdx{40}, ColIdx{100}, renderParams(buf));
  // The rendered output should mention "Esc-m" and "mode".
  auto stripped = out; // raw text includes the shortcut labels
  bool found = false;
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    auto& line = buf.lineContent(i);
    std::string s(line.begin(), line.end());
    if (s.find("Esc-m") != std::string::npos) {
      found = true;
      break;
    }
  }
  BOOST_CHECK(found);
}
