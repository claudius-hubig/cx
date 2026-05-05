#define BOOST_TEST_MODULE VersionHistory
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/version_history.hpp"

#include <memory>
#include <string>
#include <vector>

static LinePtr lp(std::string s) { return std::make_shared<std::string const>(std::move(s)); }
static std::vector<LinePtr> lps(std::initializer_list<std::string> ss) {
  std::vector<LinePtr> r;
  r.reserve(ss.size());
  for (auto& s : ss)
    r.push_back(lp(s));
  return r;
}

BOOST_AUTO_TEST_CASE(empty_history) {
  VersionHistory h;
  BOOST_CHECK(h.empty());
  BOOST_CHECK_EQUAL(h.size(), 0u);
}

BOOST_AUTO_TEST_CASE(capture_grows_history) {
  VersionHistory h;
  auto lines = lps({"hello"});
  h.capture(lines, {0, 0});
  BOOST_CHECK_EQUAL(h.size(), 1u);
  BOOST_CHECK_EQUAL(*h.at(0).lines[0], "hello");
  BOOST_CHECK_EQUAL(h.at(0).cursor.line, 0u);
  BOOST_CHECK_EQUAL(h.at(0).cursor.offset, 0u);
}

BOOST_AUTO_TEST_CASE(capture_shares_unchanged_lines) {
  VersionHistory h;
  auto lines = lps({"a", "b", "c", "d"});
  h.capture(lines, {0, 0});
  lines[1] = lp("B"); // mutate just one entry
  h.capture(lines, {1, 0});
  BOOST_CHECK_EQUAL(h.size(), 2u);
  // Unchanged lines share storage.
  BOOST_CHECK(h.at(0).lines[0].get() == h.at(1).lines[0].get());
  BOOST_CHECK(h.at(0).lines[2].get() == h.at(1).lines[2].get());
  BOOST_CHECK(h.at(0).lines[3].get() == h.at(1).lines[3].get());
  // Changed line has its own storage.
  BOOST_CHECK(h.at(0).lines[1].get() != h.at(1).lines[1].get());
  BOOST_CHECK_EQUAL(*h.at(0).lines[1], "b");
  BOOST_CHECK_EQUAL(*h.at(1).lines[1], "B");
}

BOOST_AUTO_TEST_CASE(capture_handles_line_count_change) {
  VersionHistory h;
  auto lines = lps({"a", "b"});
  h.capture(lines, {0, 0});
  lines.push_back(lp("c"));
  h.capture(lines, {2, 0});
  BOOST_CHECK_EQUAL(h.size(), 2u);
  BOOST_CHECK_EQUAL(h.at(1).lines.size(), 3u);
  BOOST_CHECK(h.at(0).lines[0].get() == h.at(1).lines[0].get());
  BOOST_CHECK(h.at(0).lines[1].get() == h.at(1).lines[1].get());
  BOOST_CHECK_EQUAL(*h.at(1).lines[2], "c");
}

BOOST_AUTO_TEST_CASE(truncate_after_drops_forward_entries) {
  VersionHistory h;
  for (std::size_t i = 0; i < 20; ++i)
    h.capture(lps({std::to_string(i)}), {0, 0});
  BOOST_CHECK_EQUAL(h.size(), 20u);
  h.truncateAfter(5);
  BOOST_CHECK_EQUAL(h.size(), 6u);
  BOOST_CHECK_EQUAL(*h.at(5).lines[0], "5");
}

BOOST_AUTO_TEST_CASE(truncate_after_last_index_is_noop) {
  VersionHistory h;
  for (std::size_t i = 0; i < 5; ++i)
    h.capture(lps({std::to_string(i)}), {0, 0});
  h.truncateAfter(4);
  BOOST_CHECK_EQUAL(h.size(), 5u);
}

BOOST_AUTO_TEST_CASE(prune_triggers_when_exceeding_cap) {
  VersionHistory h;
  // Push exactly the cap — no prune.
  for (std::size_t i = 0; i < VersionHistory::kMaxVersions; ++i)
    h.capture(lps({std::to_string(i)}), {0, 0});
  BOOST_CHECK_EQUAL(h.size(), VersionHistory::kMaxVersions);

  // One more triggers prune: thin the oldest kThinCount entries by
  // dropping every second one (indices 1, 3, 5, ...).
  h.capture(lps({std::to_string(VersionHistory::kMaxVersions)}), {0, 0});

  std::size_t thinned = VersionHistory::kThinCount / 2;
  std::size_t expected = VersionHistory::kMaxVersions + 1 - thinned;
  BOOST_CHECK_EQUAL(h.size(), expected);
  BOOST_CHECK_LE(h.size(), VersionHistory::kMaxVersions);

  // Newest entry survived.
  BOOST_CHECK_EQUAL(*h.at(h.size() - 1).lines[0], std::to_string(VersionHistory::kMaxVersions));

  // The very oldest entry (0) is always preserved.
  BOOST_CHECK_EQUAL(*h.at(0).lines[0], "0");

  // The oldest kThinCount entries were thinned: only even-indexed
  // originals survive (0, 2, 4, ...).
  std::size_t kept = VersionHistory::kThinCount - thinned;
  for (std::size_t i = 0; i < kept; ++i) {
    int val = std::stoi(*h.at(i).lines[0]);
    BOOST_CHECK_EQUAL(val, static_cast<int>(i * 2));
  }

  // The most recent entries (after the thinned region) are untouched.
  std::size_t recentStart = VersionHistory::kThinCount;
  for (std::size_t i = kept; i < h.size(); ++i) {
    int val = std::stoi(*h.at(i).lines[0]);
    BOOST_CHECK_EQUAL(val, static_cast<int>(recentStart + (i - kept)));
  }
}

BOOST_AUTO_TEST_CASE(prune_keeps_size_bounded_over_many_pushes) {
  VersionHistory h;
  for (std::size_t i = 0; i < 500; ++i) {
    h.capture(lps({std::to_string(i)}), {0, 0});
    BOOST_CHECK_LE(h.size(), VersionHistory::kMaxVersions);
  }
  // Newest is still present.
  BOOST_CHECK_EQUAL(*h.at(h.size() - 1).lines[0], "499");
}

BOOST_AUTO_TEST_CASE(clear_resets_history) {
  VersionHistory h;
  h.capture(lps({"a"}), {0, 0});
  h.capture(lps({"b"}), {0, 0});
  h.clear();
  BOOST_CHECK(h.empty());
}

// Force-materializing over-long chains during pruning (#25). Drives many
// single-line-delta captures across multiple prune cycles and verifies
// that every surviving snapshot still materializes to the expected
// content. Without the prune-time chain enforcement, sufficiently
// pathological merge patterns can leave interior deltas walking back
// further than kMaxDeltaChain entries.
BOOST_AUTO_TEST_CASE(prune_force_materializes_long_chains) {
  VersionHistory h;
  auto lines = lps({"a", "b", "c", "d", "e"});
  h.capture(lines, {0, 0});

  // Single-line edits stay on the delta path until kMaxDeltaChain forces
  // a full snapshot. Mix in occasional lineShift!=0 captures (which take
  // the full-snapshot branch) so prune meets a realistic delta/full mix.
  for (std::size_t step = 0; step < 800; ++step) {
    std::size_t idx = step % lines.size();
    lines[idx] = lp("v" + std::to_string(step));
    if (step % 17 == 0) {
      // Take the multi-line branch — exercises the full-snapshot capture.
      lines.push_back(lp("x" + std::to_string(step)));
      h.capture(lines, {0, 0}, idx, 1);
    } else {
      h.capture(lines, {0, 0}, idx, 0);
    }
    BOOST_CHECK_LE(h.size(), VersionHistory::kMaxVersions);
  }

  // Every surviving snapshot must materialize without crashing or
  // returning bad content (the trailing edits land at tail).
  for (std::size_t i = 0; i < h.size(); ++i) {
    auto const& snap = h.at(i);
    BOOST_REQUIRE(!snap.lines.empty());
    for (auto const& lp : snap.lines)
      BOOST_REQUIRE(lp != nullptr);
  }
}
