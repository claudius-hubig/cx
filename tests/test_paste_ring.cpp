#define BOOST_TEST_MODULE PasteRing
#include <boost/test/unit_test.hpp>

#include "paste_ring.hpp"

#include <string>

BOOST_AUTO_TEST_CASE(empty_ring_has_zero_size) {
  PasteRing ring;
  BOOST_CHECK(ring.empty());
  BOOST_CHECK_EQUAL(ring.size(), 0u);
}

BOOST_AUTO_TEST_CASE(push_single_entry) {
  PasteRing ring;
  ring.push("hello");
  BOOST_CHECK(!ring.empty());
  BOOST_CHECK_EQUAL(ring.size(), 1u);
  BOOST_CHECK_EQUAL(ring.at(0), "hello");
}

BOOST_AUTO_TEST_CASE(push_orders_most_recent_first) {
  PasteRing ring;
  ring.push("first");
  ring.push("second");
  ring.push("third");
  BOOST_CHECK_EQUAL(ring.size(), 3u);
  BOOST_CHECK_EQUAL(ring.at(0), "third");
  BOOST_CHECK_EQUAL(ring.at(1), "second");
  BOOST_CHECK_EQUAL(ring.at(2), "first");
}

BOOST_AUTO_TEST_CASE(push_preserves_embedded_newlines) {
  PasteRing ring;
  ring.push("line1\nline2\nline3");
  BOOST_CHECK_EQUAL(ring.at(0), "line1\nline2\nline3");
}

BOOST_AUTO_TEST_CASE(push_evicts_oldest_when_at_capacity) {
  PasteRing ring;
  for (std::size_t i = 0; i < PasteRing::kDefaultCapacity; ++i)
    ring.push(std::to_string(i));
  BOOST_CHECK_EQUAL(ring.size(), PasteRing::kDefaultCapacity);
  // Entry 0 (the very first push) is the oldest — still present.
  BOOST_CHECK_EQUAL(ring.at(PasteRing::kDefaultCapacity - 1), "0");

  // Push one more; the oldest ("0") should be evicted, and the ring
  // stays at capacity.
  ring.push("new");
  BOOST_CHECK_EQUAL(ring.size(), PasteRing::kDefaultCapacity);
  BOOST_CHECK_EQUAL(ring.at(0), "new");
  BOOST_CHECK_EQUAL(ring.at(PasteRing::kDefaultCapacity - 1), "1");
}

BOOST_AUTO_TEST_CASE(push_evicts_oldest_when_overflowing_by_many) {
  PasteRing ring;
  // Push more than capacity. Only the last kDefaultCapacity entries
  // should remain.
  std::size_t const overflow = PasteRing::kDefaultCapacity + 5;
  for (std::size_t i = 0; i < overflow; ++i)
    ring.push(std::to_string(i));

  BOOST_CHECK_EQUAL(ring.size(), PasteRing::kDefaultCapacity);
  BOOST_CHECK_EQUAL(ring.at(0), std::to_string(overflow - 1));
  BOOST_CHECK_EQUAL(ring.at(PasteRing::kDefaultCapacity - 1),
                    std::to_string(overflow - PasteRing::kDefaultCapacity));
}

BOOST_AUTO_TEST_CASE(at_throws_out_of_range_on_empty) {
  PasteRing ring;
  BOOST_CHECK_THROW(ring.at(0), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(at_throws_out_of_range_past_size) {
  PasteRing ring;
  ring.push("a");
  ring.push("b");
  BOOST_CHECK_NO_THROW(ring.at(0));
  BOOST_CHECK_NO_THROW(ring.at(1));
  BOOST_CHECK_THROW(ring.at(2), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(push_moves_string) {
  // Verify that push takes ownership via move — after the move the
  // source should be left empty.
  PasteRing ring;
  std::string source = "transferred";
  ring.push(std::move(source));
  BOOST_CHECK_EQUAL(ring.at(0), "transferred");
  // NOLINTNEXTLINE(bugprone-use-after-move) — verifying documented move behavior.
  BOOST_CHECK(source.empty());
}

BOOST_AUTO_TEST_CASE(push_allows_duplicate_entries) {
  PasteRing ring;
  ring.push("same");
  ring.push("same");
  BOOST_CHECK_EQUAL(ring.size(), 2u);
  BOOST_CHECK_EQUAL(ring.at(0), "same");
  BOOST_CHECK_EQUAL(ring.at(1), "same");
}

BOOST_AUTO_TEST_CASE(push_empty_string) {
  PasteRing ring;
  ring.push("");
  BOOST_CHECK_EQUAL(ring.size(), 1u);
  BOOST_CHECK_EQUAL(ring.at(0), "");
}

// Custom capacity: previously _capacity had no setter and was always
// kDefaultCapacity. Now constructible with an explicit cap.
BOOST_AUTO_TEST_CASE(custom_capacity_evicts_at_smaller_threshold) {
  PasteRing ring(3);
  ring.push("a");
  ring.push("b");
  ring.push("c");
  ring.push("d");
  BOOST_CHECK_EQUAL(ring.size(), 3u);
  BOOST_CHECK_EQUAL(ring.at(0), "d");
  BOOST_CHECK_EQUAL(ring.at(1), "c");
  BOOST_CHECK_EQUAL(ring.at(2), "b");
}
