#define BOOST_TEST_MODULE LineStore
#include <boost/test/unit_test.hpp>

#include "buffer/line_store.hpp"

#include <algorithm>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<LinePtr> numbered(std::size_t n, std::size_t from = 0) {
  std::vector<LinePtr> out;
  for (auto i = from; i < from + n; ++i)
    out.push_back(makeLine(std::to_string(i)));
  return out;
}

std::vector<std::string> contents(LineStore const& s) {
  std::vector<std::string> out;
  for (auto const& p : s)
    out.push_back(*p);
  return out;
}

// Every line of @p s, read by index and by iteration, equals @p model.
void checkEqual(LineStore const& s, std::vector<std::string> const& model) {
  BOOST_REQUIRE(s.invariantsHold());
  BOOST_REQUIRE_EQUAL(s.size(), model.size());
  BOOST_REQUIRE_EQUAL(s.empty(), model.empty());
  for (std::size_t i = 0; i < model.size(); ++i)
    BOOST_REQUIRE_EQUAL(*s[i], model[i]);
  BOOST_REQUIRE(contents(s) == model);
  if (!model.empty())
    BOOST_REQUIRE_EQUAL(*s.back(), model.back());
}

} // namespace

BOOST_AUTO_TEST_CASE(empty_store) {
  LineStore s(4);
  BOOST_CHECK(s.empty());
  BOOST_CHECK(s.begin() == s.end());
  BOOST_CHECK(s.invariantsHold());
  s.insert(0, makeLine("a"));
  checkEqual(s, {"a"});
  s.erase(0);
  checkEqual(s, {});
  s.push_back(makeLine("b"));
  checkEqual(s, {"b"});
  s.clear();
  checkEqual(s, {});
}

BOOST_AUTO_TEST_CASE(construct_from_lines_chunks_them) {
  auto lines = numbered(1000);
  LineStore s(lines, 4);
  std::vector<std::string> model;
  for (auto const& p : lines)
    model.push_back(*p);
  checkEqual(s, model);
  BOOST_CHECK_GE(s.chunkCount(), 1000u / 8);
  for (std::size_t i = 0; i < lines.size(); ++i)
    BOOST_CHECK(s[i] == lines[i]); // the same LinePtrs, not copies
}

// Random edits against a plain vector model, with small chunks so every
// split/merge/drop path runs often. Snapshots (copies) taken along the
// way must keep their contents whatever happens to the original.
BOOST_AUTO_TEST_CASE(random_edits_match_vector_model) {
  for (std::size_t chunk : {4u, 8u}) {
    std::mt19937 rng(1234 + chunk);
    auto pick = [&](std::size_t lo, std::size_t hi) {
      return std::uniform_int_distribution<std::size_t>(lo, hi)(rng);
    };
    LineStore s(chunk);
    std::vector<std::string> model;
    std::vector<std::pair<LineStore, std::vector<std::string>>> snaps;
    std::size_t next = 0;
    auto fresh = [&](std::size_t n) {
      auto v = numbered(n, next);
      next += n;
      return v;
    };

    for (int step = 0; step < 6000; ++step) {
      auto op = pick(0, 99);
      if (op < 15) { // insert one
        auto pos = pick(0, model.size());
        auto l = fresh(1);
        s.insert(pos, l[0]);
        model.insert(model.begin() + pos, *l[0]);
      } else if (op < 25) { // insert a range, sometimes many chunks' worth
        auto pos = pick(0, model.size());
        auto l = fresh(pick(0, 1) ? pick(0, 3 * chunk) : pick(0, 60));
        s.insert(pos, l);
        for (std::size_t k = 0; k < l.size(); ++k)
          model.insert(model.begin() + pos + k, *l[k]);
      } else if (op < 37 && !model.empty()) { // erase one
        auto pos = pick(0, model.size() - 1);
        s.erase(pos);
        model.erase(model.begin() + pos);
      } else if (op < 47 && !model.empty()) { // erase a range
        auto first = pick(0, model.size());
        auto last = std::min(model.size(), first + (pick(0, 1) ? pick(0, 3 * chunk) : pick(0, 80)));
        s.erase(first, last);
        model.erase(model.begin() + first, model.begin() + last);
      } else if (op < 60 && !model.empty()) { // replace
        auto i = pick(0, model.size() - 1);
        auto l = fresh(1);
        s.replace(i, l[0]);
        model[i] = *l[0];
      } else if (op < 78 && !model.empty()) { // edit in place
        auto i = pick(0, model.size() - 1);
        s.mutableLine(i) += "+";
        model[i] += "+";
      } else if (op < 90) { // append
        auto l = fresh(1);
        s.push_back(l[0]);
        model.push_back(*l[0]);
      } else if (op < 97) { // snapshot
        snaps.emplace_back(s, model);
        if (snaps.size() > 8)
          snaps.erase(snaps.begin());
      } else if (op < 98) {
        s.clear();
        model.clear();
      } else if (!model.empty()) { // random reads, to move the lookup cache
        for (int k = 0; k < 5; ++k) {
          auto i = pick(0, model.size() - 1);
          BOOST_REQUIRE_EQUAL(*s[i], model[i]);
        }
      }
      checkEqual(s, model);
    }
    for (auto const& [snap, expected] : snaps)
      checkEqual(snap, expected);
  }
}

BOOST_AUTO_TEST_CASE(copy_shares_chunks_until_edited) {
  LineStore s(numbered(100), 4);
  LineStore snap = s;
  for (std::size_t i = 0; i < s.size(); ++i)
    BOOST_CHECK_EQUAL(s.chunkIdentity(i), snap.chunkIdentity(i));

  s.replace(50, makeLine("x"));
  // Only the chunk holding line 50 was cloned.
  std::set<void const*> changed;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s.chunkIdentity(i) != snap.chunkIdentity(i))
      changed.insert(s.chunkIdentity(i));
    else
      BOOST_CHECK(s[i] == snap[i]);
  }
  BOOST_CHECK_EQUAL(changed.size(), 1u);
  BOOST_CHECK(changed.count(s.chunkIdentity(50)));
  BOOST_CHECK_EQUAL(*snap[50], "50");
  BOOST_CHECK_EQUAL(*s[50], "x");
}

BOOST_AUTO_TEST_CASE(mutable_line_detaches_a_shared_line) {
  LineStore s(numbered(20), 4);
  LineStore snap = s;
  auto before = snap[7];
  s.mutableLine(7) += "!";
  BOOST_CHECK_EQUAL(*s[7], "7!");
  BOOST_CHECK_EQUAL(*snap[7], "7");
  BOOST_CHECK(snap[7] == before);
  BOOST_CHECK(s[7] != before);
  // Neighbours in the cloned chunk still share their strings.
  BOOST_CHECK(s[6] == snap[6]);
  BOOST_CHECK(s[8] == snap[8]);
}

BOOST_AUTO_TEST_CASE(mutable_line_edits_an_unshared_line_in_place) {
  LineStore s(numbered(20), 4);
  auto const* before = s[7].get();
  s.mutableLine(7) += "!";
  BOOST_CHECK_EQUAL(s[7].get(), before);
  BOOST_CHECK_EQUAL(*s[7], "7!");
}

BOOST_AUTO_TEST_CASE(large_erase_keeps_untouched_chunks_shared) {
  LineStore s(numbered(400), 4);
  LineStore snap = s;
  s.erase(10, 390);
  BOOST_CHECK(s.invariantsHold());
  BOOST_CHECK_EQUAL(s.size(), 20u);
  BOOST_CHECK_EQUAL(*s[9], "9");
  BOOST_CHECK_EQUAL(*s[10], "390");
  BOOST_CHECK_EQUAL(snap.size(), 400u);
  // Lines away from the cut still sit in chunks shared with the snapshot.
  BOOST_CHECK_EQUAL(s.chunkIdentity(0), snap.chunkIdentity(0));
  BOOST_CHECK_EQUAL(s.chunkIdentity(19), snap.chunkIdentity(399));
}

BOOST_AUTO_TEST_CASE(large_insert_builds_whole_chunks) {
  LineStore s(numbered(10), 4);
  s.insert(5, numbered(1000, 100));
  BOOST_CHECK(s.invariantsHold());
  BOOST_CHECK_EQUAL(s.size(), 1010u);
  BOOST_CHECK_EQUAL(*s[4], "4");
  BOOST_CHECK_EQUAL(*s[5], "100");
  BOOST_CHECK_EQUAL(*s[1004], "1099");
  BOOST_CHECK_EQUAL(*s[1005], "5");
  BOOST_CHECK_GE(s.chunkCount(), 1010u / 8);
}

BOOST_AUTO_TEST_CASE(lookup_cache_survives_shrinking) {
  LineStore s(numbered(100), 4);
  BOOST_CHECK_EQUAL(*s[99], "99"); // cache now on the last chunk
  s.erase(3, 100);
  BOOST_CHECK_EQUAL(s.size(), 3u);
  BOOST_CHECK_EQUAL(*s[2], "2");
  s.clear();
  s.push_back(makeLine("z"));
  BOOST_CHECK_EQUAL(*s[0], "z");
}

BOOST_AUTO_TEST_CASE(push_back_after_copy_leaves_the_copy_alone) {
  LineStore s(4);
  s.push_back(makeLine("a"));
  LineStore snap = s;
  s.push_back(makeLine("b"));
  BOOST_CHECK_EQUAL(snap.size(), 1u);
  BOOST_CHECK_EQUAL(s.size(), 2u);
  BOOST_CHECK(contents(snap) == std::vector<std::string>{"a"});
}
