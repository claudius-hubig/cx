#define BOOST_TEST_MODULE PasteRingBuffer
#include <boost/test/unit_test.hpp>

#include "buffer/paste_ring_buffer.hpp"
#include "paste_ring.hpp"

BOOST_AUTO_TEST_CASE(empty_ring_shows_placeholder) {
  PasteRingBuffer buf;
  PasteRing ring;
  buf.refresh(ring);
  BOOST_CHECK_EQUAL(buf.lineCount(), 1u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "(empty)");
}

BOOST_AUTO_TEST_CASE(single_entry_shows_header_and_content) {
  PasteRingBuffer buf;
  PasteRing ring;
  ring.push("hello");
  buf.refresh(ring);
  BOOST_CHECK_EQUAL(buf.lineCount(), 2u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "--- Entry 1 ---");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "hello");
}

BOOST_AUTO_TEST_CASE(multiline_entry_splits_on_newline) {
  PasteRingBuffer buf;
  PasteRing ring;
  ring.push("line1\nline2\nline3");
  buf.refresh(ring);
  // Header + 3 content lines.
  BOOST_CHECK_EQUAL(buf.lineCount(), 4u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "--- Entry 1 ---");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "line1");
  BOOST_CHECK_EQUAL(buf.lineContent(2), "line2");
  BOOST_CHECK_EQUAL(buf.lineContent(3), "line3");
}

BOOST_AUTO_TEST_CASE(two_entries_separated_by_blank_line) {
  PasteRingBuffer buf;
  PasteRing ring;
  ring.push("second"); // pushed second = entry 1 (most recent)
  ring.push("first");  // pushed last = entry 1 (most recent)
  buf.refresh(ring);
  // Entry 1: header + "first"
  // Blank line
  // Entry 2: header + "second"
  BOOST_CHECK_EQUAL(buf.lineCount(), 5u);
  BOOST_CHECK_EQUAL(buf.lineContent(0), "--- Entry 1 ---");
  BOOST_CHECK_EQUAL(buf.lineContent(1), "first");
  BOOST_CHECK_EQUAL(buf.lineContent(2), ""); // blank separator
  BOOST_CHECK_EQUAL(buf.lineContent(3), "--- Entry 2 ---");
  BOOST_CHECK_EQUAL(buf.lineContent(4), "second");
}

BOOST_AUTO_TEST_CASE(refresh_returns_top_position) {
  PasteRingBuffer buf;
  PasteRing ring;
  ring.push("data");
  auto hint = buf.refresh(ring);
  BOOST_CHECK_EQUAL(hint.point.line, 0u);
  BOOST_CHECK_EQUAL(hint.point.offset, 0u);
}

BOOST_AUTO_TEST_CASE(refresh_replaces_previous_content) {
  PasteRingBuffer buf;
  PasteRing ring;
  ring.push("old");
  buf.refresh(ring);
  BOOST_CHECK_EQUAL(buf.lineCount(), 2u);

  ring.push("new");
  buf.refresh(ring);
  // Now two entries: "new" and "old".
  BOOST_CHECK_EQUAL(buf.lineCount(), 5u);
  BOOST_CHECK_EQUAL(buf.lineContent(1), "new");
  BOOST_CHECK_EQUAL(buf.lineContent(4), "old");
}

BOOST_AUTO_TEST_CASE(is_readonly) {
  PasteRingBuffer buf;
  BOOST_CHECK(buf.readonly());
}
