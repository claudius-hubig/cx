#define BOOST_TEST_MODULE MultiCursor
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {

// Join the buffer's lines with '\n' so tests can assert on whole content.
std::string contents(Buffer const& buf) {
  std::string s;
  for (std::size_t i = 0; i < buf.lineCount(); ++i) {
    if (i)
      s += '\n';
    s += buf.lineContent(i);
  }
  return s;
}

// All cursors (primary included) in ascending buffer order.
std::vector<BufferPoint> allCursors(BufferView const& v) {
  std::vector<BufferPoint> out(v.extraCursors().begin(), v.extraCursors().end());
  out.push_back(v.point());
  std::sort(out.begin(), out.end());
  return out;
}

} // namespace

// --- Creation ---

BOOST_AUTO_TEST_CASE(select_all_occurrences_places_cursor_after_each_match) {
  TempFile f("foo bar\nbaz foo\nfoo\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);

  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 3u);
  auto cs = allCursors(view);
  BOOST_REQUIRE_EQUAL(cs.size(), 3u);
  // Cursors sit at the *end* of each occurrence.
  BOOST_CHECK(cs[0] == (BufferPoint{0, 3}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 7}));
  BOOST_CHECK(cs[2] == (BufferPoint{2, 3}));
  // The primary is the last occurrence.
  BOOST_CHECK(view.point() == (BufferPoint{2, 3}));
}

BOOST_AUTO_TEST_CASE(select_all_occurrences_is_whole_word_only) {
  TempFile f("foo foobar barfoo foo_x foo\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);

  // Only the bare "foo" at offset 0 and the trailing one qualify:
  // foobar/barfoo/foo_x all have word bytes adjacent.
  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 2u);
  auto cs = allCursors(view);
  BOOST_REQUIRE_EQUAL(cs.size(), 2u);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 3}));
  BOOST_CHECK(cs[1] == (BufferPoint{0, 27}));
}

BOOST_AUTO_TEST_CASE(select_all_occurrences_finds_word_when_cursor_is_inside_it) {
  TempFile f("alpha beta\nbeta\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 7); // inside "beta"

  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 2u);
  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 10}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 4}));
}

BOOST_AUTO_TEST_CASE(select_all_occurrences_no_word_at_cursor) {
  TempFile f("   \nfoo\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1); // whitespace only

  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 0u);
  BOOST_CHECK(!view.multiCursor());
}

BOOST_AUTO_TEST_CASE(select_all_occurrences_uses_marked_region_as_literal) {
  TempFile f("ab abc\nxabz\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 0});
  view.setPoint(0, 2); // region "ab"

  // A marked region matches as a plain substring, not a whole word.
  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 3u);
  BOOST_CHECK(!view.mark().has_value());
  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 2}));
  BOOST_CHECK(cs[1] == (BufferPoint{0, 5}));
  BOOST_CHECK(cs[2] == (BufferPoint{1, 3}));
}

BOOST_AUTO_TEST_CASE(add_cursor_down_and_up) {
  TempFile f("aaaa\nbbbb\ncccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 2);

  BOOST_CHECK(view.addCursorDown());
  BOOST_CHECK_EQUAL(view.cursorCount(), 2u);
  BOOST_CHECK(view.addCursorUp());
  BOOST_CHECK_EQUAL(view.cursorCount(), 3u);

  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 2}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 2}));
  BOOST_CHECK(cs[2] == (BufferPoint{2, 2}));
  // The primary never moves when cursors are added around it.
  BOOST_CHECK(view.point() == (BufferPoint{1, 2}));
}

BOOST_AUTO_TEST_CASE(add_cursor_down_clamps_to_short_lines) {
  TempFile f("abcdef\nxy\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 5);

  BOOST_CHECK(view.addCursorDown());
  auto cs = allCursors(view);
  BOOST_CHECK(cs[1] == (BufferPoint{1, 2})); // clamped to end of "xy"
}

BOOST_AUTO_TEST_CASE(add_cursor_stops_at_buffer_edges) {
  TempFile f("only\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  // Line 1 exists (the empty trailing line from the final newline), so
  // down succeeds once and then runs out.
  BOOST_CHECK(view.addCursorDown());
  BOOST_CHECK(!view.addCursorDown());
  BOOST_CHECK(!view.addCursorUp());
}

BOOST_AUTO_TEST_CASE(adding_a_cursor_on_the_primary_merges) {
  TempFile f("aaa\nbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);

  view.addCursor({0, 1}); // same as primary
  BOOST_CHECK(!view.multiCursor());
  view.addCursor({1, 1});
  view.addCursor({1, 1}); // duplicate extra
  BOOST_CHECK_EQUAL(view.cursorCount(), 2u);
}

// --- Editing ---

BOOST_AUTO_TEST_CASE(insert_at_two_cursors_on_one_line_shifts_offsets) {
  TempFile f("abcdef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({0, 4});

  view.insert('X');
  BOOST_CHECK_EQUAL(buf.lineContent(0), "aXbcdXef");
  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 2}));
  // The second cursor shifted right by the first insertion.
  BOOST_CHECK(cs[1] == (BufferPoint{0, 6}));
}

BOOST_AUTO_TEST_CASE(insert_at_cursors_on_different_lines) {
  TempFile f("one\ntwo\nthree\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.addCursor({1, 0});
  view.addCursor({2, 0});

  view.insert('>');
  BOOST_CHECK_EQUAL(contents(buf), ">one\n>two\n>three\n");
  BOOST_CHECK_EQUAL(view.cursorCount(), 3u);
}

BOOST_AUTO_TEST_CASE(insert_at_buffer_start_and_end) {
  TempFile f("ab\ncd");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.addCursor({1, 2});

  view.insert('!');
  BOOST_CHECK_EQUAL(contents(buf), "!ab\ncd!");
}

BOOST_AUTO_TEST_CASE(newline_at_several_cursors_shifts_lines) {
  TempFile f("ab\ncd\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.insert('\n');
  BOOST_CHECK_EQUAL(contents(buf), "a\nb\nc\nd\n");
  auto cs = allCursors(view);
  BOOST_REQUIRE_EQUAL(cs.size(), 2u);
  BOOST_CHECK(cs[0] == (BufferPoint{1, 0}));
  // The lower cursor moved down by the line the upper one added.
  BOOST_CHECK(cs[1] == (BufferPoint{3, 0}));
}

BOOST_AUTO_TEST_CASE(utf8_insert_at_every_cursor) {
  TempFile f("aa\nbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.insert(0x00E4); // 'ä', two UTF-8 bytes
  BOOST_CHECK_EQUAL(contents(buf), "a\xc3\xa4\x61\nb\xc3\xa4\x62\n");
  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 3}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 3}));
}

BOOST_AUTO_TEST_CASE(overwrite_mode_replaces_at_every_cursor) {
  TempFile f("abc\ndef\n");
  TextBuffer buf(f.path().string(), f.path());
  buf.setOverwriteMode(true);
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.insert('Z');
  BOOST_CHECK_EQUAL(contents(buf), "aZc\ndZf\n");
}

BOOST_AUTO_TEST_CASE(backspace_at_every_cursor) {
  TempFile f("abc\ndef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2);
  view.addCursor({1, 2});

  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(contents(buf), "ac\ndf\n");
}

BOOST_AUTO_TEST_CASE(backspace_joining_lines_moves_lower_cursors_up) {
  TempFile f("ab\ncd\nef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(1, 0); // at column 0: backspace joins line 1 onto line 0
  view.addCursor({2, 1});

  view.deleteBeforePoint();
  // Line 1 joined onto line 0; the lower cursor deleted its own 'e'.
  BOOST_CHECK_EQUAL(contents(buf), "abcd\nf\n");
  auto cs = allCursors(view);
  BOOST_REQUIRE_EQUAL(cs.size(), 2u);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 2}));
  // The cursor below also shifted up one line from the join.
  BOOST_CHECK(cs[1] == (BufferPoint{1, 0}));
}

BOOST_AUTO_TEST_CASE(delete_forward_at_every_cursor) {
  TempFile f("abc\ndef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.deleteAtPoint();
  BOOST_CHECK_EQUAL(contents(buf), "ac\ndf\n");
}

BOOST_AUTO_TEST_CASE(chunk_delete_back_kills_the_word_at_every_cursor) {
  // The headline workflow: Esc-* then Ctrl-Bksp then type a replacement.
  TempFile f("foo bar\nbaz foo\nfoo end\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);

  BOOST_CHECK_EQUAL(view.selectAllOccurrences(), 3u);
  view.deleteChunkBeforePoint();
  BOOST_CHECK_EQUAL(contents(buf), " bar\nbaz \n end\n");

  view.insert('q');
  view.insert('x');
  BOOST_CHECK_EQUAL(contents(buf), "qx bar\nbaz qx\nqx end\n");
}

BOOST_AUTO_TEST_CASE(chunk_delete_forward_at_every_cursor) {
  TempFile f("one two\nthree four\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.addCursor({1, 0});

  view.deleteChunkAtPoint();
  BOOST_CHECK_EQUAL(contents(buf), " two\n four\n");
}

BOOST_AUTO_TEST_CASE(paste_inserts_the_same_text_at_all_cursors) {
  TempFile f("a\nb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.insertText("<>");
  BOOST_CHECK_EQUAL(contents(buf), "a<>\nb<>\n");
}

BOOST_AUTO_TEST_CASE(multiline_paste_at_all_cursors) {
  TempFile f("a\nb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.insertText("X\nY");
  BOOST_CHECK_EQUAL(contents(buf), "aX\nY\nbX\nY\n");
}

BOOST_AUTO_TEST_CASE(readonly_buffer_rejects_multi_cursor_edits) {
  TempFile f("abc\ndef\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});
  buf.setBrowsingHistory(true); // makes the buffer readonly

  view.insert('X');
  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(contents(buf), "abc\ndef\n");
}

// --- Merging and clamping ---

BOOST_AUTO_TEST_CASE(cursors_that_collide_merge_into_one) {
  TempFile f("ab\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({0, 2});

  // Both delete their preceding char; the second lands where the first is.
  view.deleteBeforePoint();
  BOOST_CHECK_EQUAL(buf.lineContent(0), "");
  BOOST_CHECK_EQUAL(view.cursorCount(), 1u);
  BOOST_CHECK(!view.multiCursor());
}

BOOST_AUTO_TEST_CASE(external_edit_clamps_extra_cursors) {
  TempFile f("aaaa\nbbbb\ncccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 3);
  view.addCursor({2, 4});

  // Shrink the buffer behind the view's back, as a second view would.
  buf.deleteRegion({1, 0}, {3, 0});
  view.clampToBuffer();

  for (auto const& c : view.extraCursors()) {
    BOOST_CHECK_LT(c.line, buf.lineCount());
    BOOST_CHECK_LE(c.offset, buf.lineContent(c.line).size());
  }
}

// --- Parallel movement ---

BOOST_AUTO_TEST_CASE(movement_moves_all_cursors_in_parallel) {
  TempFile f("abcd\nefgh\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.moveRight();
  auto cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 2}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 2}));

  view.moveEnd();
  cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 4}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 4}));

  view.moveHome();
  cs = allCursors(view);
  BOOST_CHECK(cs[0] == (BufferPoint{0, 0}));
  BOOST_CHECK(cs[1] == (BufferPoint{1, 0}));
}

BOOST_AUTO_TEST_CASE(vertical_movement_keeps_each_cursors_own_column) {
  TempFile f("abcdef\nghijkl\nmnopqr\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 4});

  view.moveDown(ColIdx{80});
  auto cs = allCursors(view);
  BOOST_REQUIRE_EQUAL(cs.size(), 2u);
  BOOST_CHECK(cs[0] == (BufferPoint{1, 1}));
  BOOST_CHECK(cs[1] == (BufferPoint{2, 4}));
}

BOOST_AUTO_TEST_CASE(cursors_merging_during_movement_are_deduplicated) {
  TempFile f("abc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2);
  view.addCursor({0, 3});

  view.moveEnd(); // both land on offset 3
  BOOST_CHECK_EQUAL(view.cursorCount(), 1u);
}

// --- Collapsing ---

BOOST_AUTO_TEST_CASE(setting_the_mark_collapses_to_the_primary) {
  TempFile f("aaa\nbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});

  view.toggleMark();
  BOOST_CHECK(!view.multiCursor());
  BOOST_CHECK(view.mark().has_value());
}

BOOST_AUTO_TEST_CASE(absolute_jumps_collapse_to_the_primary) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.addCursor({1, 1});
  BOOST_CHECK(view.multiCursor());

  view.setPoint(2, 0); // find / bookmark / mouse click all land here
  BOOST_CHECK(!view.multiCursor());
}

BOOST_AUTO_TEST_CASE(goto_line_and_paging_collapse) {
  TempFile f("a\nb\nc\nd\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);

  view.addCursor({1, 0});
  view.gotoLine(3, RowIdx{10});
  BOOST_CHECK(!view.multiCursor());

  view.addCursor({1, 0});
  view.pageDown(RowIdx{2}, ColIdx{80}, false);
  BOOST_CHECK(!view.multiCursor());
}

BOOST_AUTO_TEST_CASE(history_mode_collapses) {
  TempFile f("aaa\nbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 1);
  view.insert('X'); // give the history something to hold
  view.addCursor({1, 1});
  BOOST_CHECK(view.multiCursor());

  view.enterHistoryMode();
  BOOST_CHECK(!view.multiCursor());
}

BOOST_AUTO_TEST_CASE(extra_cursors_are_not_saved_across_views) {
  TempFile f("aaa\nbbb\n");
  TextBuffer buf(f.path().string(), f.path());
  {
    BufferView view(buf);
    view.setPoint(0, 1);
    view.addCursor({1, 1});
    view.saveViewState();
  }
  BufferView restored = BufferView::fromSavedState(buf);
  BOOST_CHECK(!restored.multiCursor());
  BOOST_CHECK(restored.point() == (BufferPoint{0, 1}));
}

// --- Undo / version history ---

BOOST_AUTO_TEST_CASE(one_multi_cursor_keystroke_is_one_version) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.addCursor({1, 0});
  view.addCursor({2, 0});

  auto before = buf.versionHistoryInfo().size;
  view.insert('X');
  BOOST_CHECK_EQUAL(buf.versionHistoryInfo().size, before + 1);
}

BOOST_AUTO_TEST_CASE(undo_restores_all_edit_sites) {
  // The correctness test for §3.3: a scattered edit must never take the
  // single-dirty-line delta path, or reconstruction silently loses edits.
  TempFile f("aaa\nbbb\nccc\nddd\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  auto original = contents(buf);

  view.setPoint(0, 1);
  view.addCursor({1, 1});
  view.addCursor({3, 1});
  view.insert('Z');
  BOOST_CHECK_EQUAL(contents(buf), "aZaa\nbZbb\nccc\ndZdd\n");

  auto info = buf.versionHistoryInfo();
  BOOST_REQUIRE_GE(info.size, 2u);
  BufferPoint cursor;
  buf.applyVersionAt(info.size - 2, cursor);
  BOOST_CHECK_EQUAL(contents(buf), original);
}

BOOST_AUTO_TEST_CASE(undo_restores_a_multi_cursor_newline_edit) {
  TempFile f("aaa\nbbb\nccc\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  auto original = contents(buf);

  view.setPoint(0, 1);
  view.addCursor({2, 1});
  view.insert('\n');
  BOOST_CHECK_EQUAL(contents(buf), "a\naa\nbbb\nc\ncc\n");

  auto info = buf.versionHistoryInfo();
  BufferPoint cursor;
  buf.applyVersionAt(info.size - 2, cursor);
  BOOST_CHECK_EQUAL(contents(buf), original);
}

BOOST_AUTO_TEST_CASE(a_wholly_no_op_multi_cursor_edit_records_no_version) {
  TempFile f("ab");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  // Every cursor sits at the very end of a one-line buffer, so a forward
  // delete has nothing to remove anywhere.
  view.setPoint(0, 2);
  view.addCursor({0, 1});
  view.moveEnd(); // both now at offset 2 — they merge
  view.addCursor({0, 1});

  auto before = buf.versionHistoryInfo().size;
  view.setPoint(0, 2);
  view.collapseCursors();
  view.addCursor({0, 2}); // merges into the primary: a single cursor at EOF
  view.deleteAtPoint();
  BOOST_CHECK_EQUAL(contents(buf), "ab");
  BOOST_CHECK_EQUAL(buf.versionHistoryInfo().size, before);
}

BOOST_AUTO_TEST_CASE(a_partly_no_op_multi_cursor_edit_still_records_once) {
  TempFile f("ab");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 2); // end of buffer: nothing to delete here
  view.addCursor({0, 0});

  auto before = buf.versionHistoryInfo().size;
  view.deleteAtPoint();
  BOOST_CHECK_EQUAL(contents(buf), "b");
  BOOST_CHECK_EQUAL(buf.versionHistoryInfo().size, before + 1);
}

// --- The mark does not survive adding cursors ---

BOOST_AUTO_TEST_CASE(add_cursor_clears_mark) {
  TextBuffer buf("t");
  buf.insertText("short\nlonger line", BufferPoint{0, 0});
  BufferView v(buf);
  v.setPoint(1, 11);
  v.toggleMark();
  BOOST_REQUIRE(v.addCursorUp());
  BOOST_CHECK(!v.mark().has_value());
}

BOOST_AUTO_TEST_CASE(region_command_after_multi_cursor_edit_does_not_throw) {
  // Mark at (1,11), add a cursor above, Enter at both cursors: the mark was
  // left at (1,11) on what is now an empty line, and Esc-U then threw
  // std::out_of_range from Buffer::extractRegion(), ending the session.
  TextBuffer buf("t");
  buf.insertText("short\nlonger line", BufferPoint{0, 0});
  BufferView v(buf);
  v.setPoint(1, 11);
  v.toggleMark();
  BOOST_REQUIRE(v.addCursorUp());
  v.insert('\n');
  BOOST_CHECK(!v.mark().has_value());
  BOOST_CHECK_NO_THROW(v.convertCase(BufferView::CaseOp::Upper));
}

// --- Lazy batch shifting matches shifting every cursor after every edit ---

namespace {

// Reference model: the straightforward O(N^2) algorithm the batch code used
// to run — after every primitive edit, shift every cursor. ASCII only.
struct RefModel {
  std::vector<std::string> lines;
  std::vector<BufferPoint> cur; // sorted

  static BufferPoint shiftIns(BufferPoint p, BufferPoint at, BufferPoint end) {
    if (p < at)
      return p;
    if (p.line == at.line)
      return {end.line, end.offset + (p.offset - at.offset)};
    return {p.line + end.line - at.line, p.offset};
  }
  static BufferPoint shiftDel(BufferPoint p, BufferPoint from, BufferPoint to) {
    if (p <= from)
      return p;
    if (p < to)
      return from;
    if (p.line == to.line)
      return {from.line, from.offset + (p.offset - to.offset)};
    return {p.line - (to.line - from.line), p.offset};
  }
  BufferPoint insertText(BufferPoint at, std::string const& text) {
    auto tail = lines[at.line].substr(at.offset);
    lines[at.line].resize(at.offset);
    BufferPoint end = at;
    for (char c : text) {
      if (c == '\n') {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(end.line) + 1, "");
        end = {end.line + 1, 0};
      } else {
        lines[end.line] += c;
        ++end.offset;
      }
    }
    lines[end.line] += tail;
    for (auto& c : cur)
      c = shiftIns(c, at, end);
    return end;
  }
  void erase(BufferPoint from, BufferPoint to) {
    lines[from.line] = lines[from.line].substr(0, from.offset) + lines[to.line].substr(to.offset);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(from.line) + 1,
                lines.begin() + static_cast<std::ptrdiff_t>(to.line) + 1);
    for (auto& c : cur)
      c = shiftDel(c, from, to);
  }
  bool delForward(std::size_t i) {
    auto p = cur[i];
    if (p.offset < lines[p.line].size())
      erase(p, {p.line, p.offset + 1});
    else if (p.line + 1 < lines.size())
      erase(p, {p.line + 1, 0});
    else
      return false;
    return true;
  }
  bool delBackward(std::size_t i) {
    auto p = cur[i];
    if (p.offset > 0)
      erase({p.line, p.offset - 1}, p);
    else if (p.line > 0)
      erase({p.line - 1, lines[p.line - 1].size()}, p);
    else
      return false;
    return true;
  }
  bool blankAt(BufferPoint p) const {
    return p.offset >= lines[p.line].size() || lines[p.line][p.offset] == ' ';
  }
  bool blankBefore(BufferPoint p) const {
    return p.offset == 0 || lines[p.line][p.offset - 1] == ' ';
  }
  template <typename F> void each(F f) {
    for (std::size_t i = 0; i < cur.size(); ++i)
      if (i == 0 || cur[i] != cur[i - 1])
        f(i);
  }
  void op(int kind, std::string const& text) {
    switch (kind) {
    case 0:
      each([&](std::size_t i) { insertText(cur[i], text); });
      break;
    case 1:
      each([&](std::size_t i) { delForward(i); });
      break;
    case 2:
      each([&](std::size_t i) { delBackward(i); });
      break;
    case 3:
      each([&](std::size_t i) {
        bool b = blankAt(cur[i]);
        while (delForward(i) && blankAt(cur[i]) == b) {
        }
      });
      break;
    case 4:
      each([&](std::size_t i) {
        bool b = blankBefore(cur[i]);
        while (delBackward(i) && blankBefore(cur[i]) == b) {
        }
      });
      break;
    }
    std::sort(cur.begin(), cur.end());
    cur.erase(std::unique(cur.begin(), cur.end()), cur.end());
  }
};

} // namespace

BOOST_AUTO_TEST_CASE(batch_edits_match_reference_model) {
  std::mt19937 rng(12345);
  auto R = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
  char const* texts[] = {"x", "\n", "ab", "a\nb", " ", "\n\n"};
  for (int round = 0; round < 300; ++round) {
    std::string text;
    for (std::size_t n = 1 + R(40); n; --n)
      text += "ab \n"[R(4)];
    TextBuffer buf("t");
    buf.insertText(text, BufferPoint{0, 0});
    RefModel ref;
    for (std::size_t li = 0; li < buf.lineCount(); ++li)
      ref.lines.push_back(buf.lineContent(li));

    BufferView v(buf);
    auto randomPoint = [&] {
      std::size_t li = R(buf.lineCount());
      return BufferPoint{li, R(buf.lineContent(li).size() + 1)};
    };
    v.setPoint(randomPoint().line, 0);
    for (std::size_t n = 1 + R(8); n; --n)
      v.addCursor(randomPoint());

    for (int step = 0; step < 6 && v.multiCursor(); ++step) {
      ref.cur = allCursors(v);
      int kind = static_cast<int>(R(5));
      std::string t = texts[R(std::size(texts))];
      ref.op(kind, t);
      switch (kind) {
      case 0:
        if (t.size() == 1)
          v.insert(static_cast<unsigned char>(t[0]));
        else
          v.insertText(t);
        break;
      case 1:
        v.deleteAtPoint();
        break;
      case 2:
        v.deleteBeforePoint();
        break;
      case 3:
        v.deleteChunkAtPoint();
        break;
      case 4:
        v.deleteChunkBeforePoint();
        break;
      }
      std::string expected;
      for (std::size_t li = 0; li < ref.lines.size(); ++li)
        expected += (li ? "\n" : "") + ref.lines[li];
      BOOST_REQUIRE_EQUAL(contents(buf), expected);
      BOOST_REQUIRE(allCursors(v) == ref.cur);
    }
  }
}
