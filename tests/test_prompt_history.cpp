#define BOOST_TEST_MODULE PromptHistory
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "prompt.hpp"

// --- push and get basics ---

BOOST_AUTO_TEST_CASE(get_returns_nullptr_for_unknown_label) {
  PromptHistory h;
  BOOST_CHECK(h.get("Go to line: ") == nullptr);
}

BOOST_AUTO_TEST_CASE(push_then_get_returns_entries) {
  PromptHistory h;
  h.push("Find: ", "hello");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 1u);
  BOOST_CHECK_EQUAL((*vec)[0], "hello");
}

BOOST_AUTO_TEST_CASE(push_orders_most_recent_first) {
  PromptHistory h;
  h.push("Find: ", "alpha");
  h.push("Find: ", "beta");
  h.push("Find: ", "gamma");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 3u);
  BOOST_CHECK_EQUAL((*vec)[0], "gamma");
  BOOST_CHECK_EQUAL((*vec)[1], "beta");
  BOOST_CHECK_EQUAL((*vec)[2], "alpha");
}

// --- deduplication ---

BOOST_AUTO_TEST_CASE(push_deduplicates_and_moves_to_front) {
  PromptHistory h;
  h.push("Find: ", "alpha");
  h.push("Find: ", "beta");
  h.push("Find: ", "alpha"); // re-push
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 2u);
  BOOST_CHECK_EQUAL((*vec)[0], "alpha");
  BOOST_CHECK_EQUAL((*vec)[1], "beta");
}

// --- empty string rejection ---

BOOST_AUTO_TEST_CASE(push_ignores_empty_strings) {
  PromptHistory h;
  h.push("Find: ", "");
  BOOST_CHECK(h.get("Find: ") == nullptr);
}

BOOST_AUTO_TEST_CASE(push_empty_does_not_affect_existing) {
  PromptHistory h;
  h.push("Find: ", "hello");
  h.push("Find: ", "");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 1u);
  BOOST_CHECK_EQUAL((*vec)[0], "hello");
}

// --- size cap ---

BOOST_AUTO_TEST_CASE(push_caps_at_max_per_label) {
  PromptHistory h;
  for (std::size_t i = 0; i < PromptHistory::kMaxPerLabel + 10; ++i)
    h.push("Find: ", "entry_" + std::to_string(i));
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_CHECK_EQUAL(vec->size(), PromptHistory::kMaxPerLabel);
  // Most recent is first.
  BOOST_CHECK_EQUAL((*vec)[0], "entry_" + std::to_string(PromptHistory::kMaxPerLabel + 9));
  // Oldest surviving entry.
  BOOST_CHECK_EQUAL((*vec)[PromptHistory::kMaxPerLabel - 1], "entry_10");
}

// --- label isolation ---

BOOST_AUTO_TEST_CASE(separate_labels_are_independent) {
  PromptHistory h;
  h.push("Find: ", "search_term");
  h.push("Go to line: ", "42");
  auto* findVec = h.get("Find: ");
  auto* gotoVec = h.get("Go to line: ");
  BOOST_REQUIRE(findVec != nullptr);
  BOOST_REQUIRE(gotoVec != nullptr);
  BOOST_CHECK_EQUAL(findVec->size(), 1u);
  BOOST_CHECK_EQUAL(gotoVec->size(), 1u);
  BOOST_CHECK_EQUAL((*findVec)[0], "search_term");
  BOOST_CHECK_EQUAL((*gotoVec)[0], "42");
}
