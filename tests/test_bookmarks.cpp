#define BOOST_TEST_MODULE Bookmarks
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "bookmarks.hpp"

BOOST_AUTO_TEST_CASE(valid_slot_accepts_digits_and_lowercase) {
  for (char c = '0'; c <= '9'; ++c)
    BOOST_CHECK(Bookmarks::validSlot(c));
  for (char c = 'a'; c <= 'z'; ++c)
    BOOST_CHECK(Bookmarks::validSlot(c));
}

BOOST_AUTO_TEST_CASE(valid_slot_rejects_other_bytes) {
  BOOST_CHECK(!Bookmarks::validSlot(' '));
  BOOST_CHECK(!Bookmarks::validSlot('A'));
  BOOST_CHECK(!Bookmarks::validSlot('?'));
  BOOST_CHECK(!Bookmarks::validSlot(static_cast<char>(0x7F)));
}

BOOST_AUTO_TEST_CASE(get_on_empty_slot_returns_nullopt) {
  Bookmarks bm;
  BOOST_CHECK(!bm.get('a').has_value());
}

BOOST_AUTO_TEST_CASE(contains_tracks_occupancy) {
  Bookmarks bm;
  BOOST_CHECK(!bm.contains('a'));
  bm.set('a', "main.cpp", BufferPoint{3, 5});
  BOOST_CHECK(bm.contains('a'));
  BOOST_CHECK(!bm.contains('b'));
  bm.erase('a');
  BOOST_CHECK(!bm.contains('a'));
}

BOOST_AUTO_TEST_CASE(set_then_get_round_trips) {
  Bookmarks bm;
  bm.set('a', "main.cpp", BufferPoint{3, 5});
  auto mark = bm.get('a');
  BOOST_REQUIRE(mark.has_value());
  BOOST_CHECK_EQUAL(mark->bufferName, "main.cpp");
  BOOST_CHECK(mark->point == (BufferPoint{3, 5}));
}

BOOST_AUTO_TEST_CASE(set_on_occupied_slot_overwrites) {
  Bookmarks bm;
  bm.set('a', "main.cpp", BufferPoint{3, 5});
  bm.set('a', "todo.md", BufferPoint{0, 0});
  auto mark = bm.get('a');
  BOOST_REQUIRE(mark.has_value());
  BOOST_CHECK_EQUAL(mark->bufferName, "todo.md");
  BOOST_CHECK(mark->point == (BufferPoint{0, 0}));
}

BOOST_AUTO_TEST_CASE(erase_empties_the_slot) {
  Bookmarks bm;
  bm.set('a', "main.cpp", BufferPoint{3, 5});
  bm.erase('a');
  BOOST_CHECK(!bm.get('a').has_value());
}

BOOST_AUTO_TEST_CASE(erase_on_empty_slot_is_a_no_op) {
  Bookmarks bm;
  bm.erase('a'); // must not throw or crash
  BOOST_CHECK(!bm.get('a').has_value());
}

BOOST_AUTO_TEST_CASE(rename_buffer_rekeys_matching_entries_only) {
  Bookmarks bm;
  bm.set('a', "old.cpp", BufferPoint{1, 0});
  bm.set('b', "other.cpp", BufferPoint{2, 0});
  bm.renameBuffer("old.cpp", "new.cpp");

  auto a = bm.get('a');
  BOOST_REQUIRE(a.has_value());
  BOOST_CHECK_EQUAL(a->bufferName, "new.cpp");

  auto b = bm.get('b');
  BOOST_REQUIRE(b.has_value());
  BOOST_CHECK_EQUAL(b->bufferName, "other.cpp");
}

BOOST_AUTO_TEST_CASE(rename_buffer_with_no_matches_is_a_no_op) {
  Bookmarks bm;
  bm.set('a', "other.cpp", BufferPoint{1, 0});
  bm.renameBuffer("old.cpp", "new.cpp");
  auto a = bm.get('a');
  BOOST_REQUIRE(a.has_value());
  BOOST_CHECK_EQUAL(a->bufferName, "other.cpp");
}

BOOST_AUTO_TEST_CASE(list_is_empty_for_a_fresh_store) {
  Bookmarks bm;
  BOOST_CHECK(bm.list().empty());
}

BOOST_AUTO_TEST_CASE(list_returns_occupied_slots_in_sorted_order) {
  Bookmarks bm;
  bm.set('z', "z.cpp", BufferPoint{0, 0});
  bm.set('1', "one.cpp", BufferPoint{0, 0});
  bm.set('a', "a.cpp", BufferPoint{0, 0});

  auto entries = bm.list();
  BOOST_REQUIRE_EQUAL(entries.size(), 3u);
  BOOST_CHECK_EQUAL(entries[0].first, '1');
  BOOST_CHECK_EQUAL(entries[1].first, 'a');
  BOOST_CHECK_EQUAL(entries[2].first, 'z');
}

BOOST_AUTO_TEST_CASE(list_reflects_erase) {
  Bookmarks bm;
  bm.set('a', "a.cpp", BufferPoint{0, 0});
  bm.set('b', "b.cpp", BufferPoint{0, 0});
  bm.erase('a');
  auto entries = bm.list();
  BOOST_REQUIRE_EQUAL(entries.size(), 1u);
  BOOST_CHECK_EQUAL(entries[0].first, 'b');
}
