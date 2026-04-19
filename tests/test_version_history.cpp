#define BOOST_TEST_MODULE VersionHistory
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/version_history.hpp"

#include <string>
#include <vector>

BOOST_AUTO_TEST_CASE(empty_history) {
  VersionHistory h;
  BOOST_CHECK(h.empty());
  BOOST_CHECK_EQUAL(h.size(), 0u);
}

BOOST_AUTO_TEST_CASE(capture_grows_history) {
  VersionHistory h;
  std::vector<std::string> lines{"hello"};
  h.capture(lines, {0, 0});
  BOOST_CHECK_EQUAL(h.size(), 1u);
  BOOST_CHECK_EQUAL(*h.at(0).lines[0], "hello");
  BOOST_CHECK_EQUAL(h.at(0).cursor.line, 0u);
  BOOST_CHECK_EQUAL(h.at(0).cursor.offset, 0u);
}

BOOST_AUTO_TEST_CASE(capture_shares_unchanged_lines) {
  VersionHistory h;
  std::vector<std::string> lines{"a", "b", "c", "d"};
  h.capture(lines, {0, 0});
  lines[1] = "B";
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
  std::vector<std::string> lines{"a", "b"};
  h.capture(lines, {0, 0});
  lines.push_back("c");
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
    h.capture({std::to_string(i)}, {0, 0});
  BOOST_CHECK_EQUAL(h.size(), 20u);
  h.truncateAfter(5);
  BOOST_CHECK_EQUAL(h.size(), 6u);
  BOOST_CHECK_EQUAL(*h.at(5).lines[0], "5");
}

BOOST_AUTO_TEST_CASE(truncate_after_last_index_is_noop) {
  VersionHistory h;
  for (std::size_t i = 0; i < 5; ++i)
    h.capture({std::to_string(i)}, {0, 0});
  h.truncateAfter(4);
  BOOST_CHECK_EQUAL(h.size(), 5u);
}

BOOST_AUTO_TEST_CASE(prune_triggers_when_exceeding_cap) {
  VersionHistory h;
  // Push exactly the cap — no prune.
  for (std::size_t i = 0; i < VersionHistory::kMaxVersions; ++i)
    h.capture({std::to_string(i)}, {0, 0});
  BOOST_CHECK_EQUAL(h.size(), VersionHistory::kMaxVersions);

  // One more triggers prune: thin the oldest kThinCount entries by
  // dropping every second one (indices 1, 3, 5, ...).
  h.capture({std::to_string(VersionHistory::kMaxVersions)}, {0, 0});

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
    h.capture({std::to_string(i)}, {0, 0});
    BOOST_CHECK_LE(h.size(), VersionHistory::kMaxVersions);
  }
  // Newest is still present.
  BOOST_CHECK_EQUAL(*h.at(h.size() - 1).lines[0], "499");
}

BOOST_AUTO_TEST_CASE(clear_resets_history) {
  VersionHistory h;
  h.capture({"a"}, {0, 0});
  h.capture({"b"}, {0, 0});
  h.clear();
  BOOST_CHECK(h.empty());
}
