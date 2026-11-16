#define BOOST_TEST_MODULE MdTable
#include <boost/test/unit_test.hpp>

#include "buffer/buffer_view.hpp"
#include "buffer/modes/md/fence.hpp"
#include "buffer/modes/md/table.hpp"
#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <ranges>

namespace {

std::vector<std::string> splitLines(std::string_view s) {
  std::vector<std::string> out;
  for (auto part : std::views::split(s, '\n'))
    out.emplace_back(std::string_view(part));
  return out;
}

std::string joinLines(std::vector<std::string> const& lines) {
  std::string out;
  for (auto const& l : lines)
    out += (out.empty() ? "" : "\n") + l;
  return out;
}

std::string contents(Buffer const& buf) {
  std::vector<std::string> lines;
  for (std::size_t i = 0; i < buf.lineCount(); ++i)
    lines.push_back(buf.lineContent(i));
  return joinLines(lines);
}

/** @brief Cursor on the first character of column @p col's header text. */
BufferPoint headerCell(std::string const& header, std::size_t col) {
  std::size_t off = header.find('|');
  for (std::size_t c = 0; c < col; ++c)
    off = header.find('|', off + 1);
  return {0, header.find_first_not_of(' ', off + 1)};
}

/** @brief Format @p table, wrapping column @p col (negative: align only). */
std::string format(std::string_view table, int col) {
  auto lines = splitLines(table);
  std::optional<BufferPoint> cursor;
  if (col >= 0)
    cursor = headerCell(lines[0], static_cast<std::size_t>(col));
  auto result = formatMarkdownTable(lines, cursor, 80);
  BOOST_REQUIRE(result);
  return joinLines(result->lines);
}

struct Example {
  char const* name;
  int column; // column to wrap; -1 aligns only
  char const* input;
  char const* expected;
};

// The examples from the design discussion; expected output was produced
// by an independent prototype of the same algorithm.
Example const kExamples[] = {
    {"Align only (cursor in first column)", -1,
     R"(| Key | Action |
|---|---|
| Alt-q | Reflow paragraph |
| C-x C-s | Save |
| C-g | Cancel |)",
     R"(| Key     | Action           |
| ------- | ---------------- |
| Alt-q   | Reflow paragraph |
| C-x C-s | Save             |
| C-g     | Cancel           |)"},
    {"Alignment markers are honoured", -1,
     R"(| Item | Qty | Price | Note |
|:--|:-:|--:|---|
| Apple | 3 | 1.20 | fresh |
| Watermelon | 12 | 15.00 | |
| Fig | 100 | 0.5 | dried, imported |)",
     R"(| Item       | Qty | Price | Note            |
| :--------- | :-: | ----: | --------------- |
| Apple      |  3  |  1.20 | fresh           |
| Watermelon | 12  | 15.00 |                 |
| Fig        | 100 |   0.5 | dried, imported |)"},
    {"Wrap the last column", 2,
     R"(| Option | Default | Description |
|---|---|---|
| `--lines N` | 50000 | Number of lines in the synthetic document that the benchmark builds before it starts timing any of the inserts. |
| `--inserts N` | 200 | How many single-character inserts to time. |
| `--direct` | off | Call BufferView directly instead of going through Screen, which removes rendering from the measurement entirely. |)",
     R"(| Option        | Default | Description                                        |
| ------------- | ------- | -------------------------------------------------- |
| `--lines N`   | 50000   | Number of lines in the synthetic document that the |
|               |         | benchmark builds before it starts timing any of    |
|               |         | the inserts.                                       |
| `--inserts N` | 200     | How many single-character inserts to time.         |
| `--direct`    | off     | Call BufferView directly instead of going through  |
|               |         | Screen, which removes rendering from the           |
|               |         | measurement entirely.                              |)"},
    {"Wrap a middle column", 1,
     R"(| Mode | Detection rule | Tokenizer |
|---|---|---|
| md | File extension is `.md` or `.markdown`, or the first line starts with a `#` heading followed by a blank line. | md/syntax.cpp |
| py | Extension `.py`, or a shebang mentioning python. | py/syntax.cpp |)",
     R"(| Mode | Detection rule                                       | Tokenizer     |
| ---- | ---------------------------------------------------- | ------------- |
| md   | File extension is `.md` or `.markdown`, or the first | md/syntax.cpp |
|      | line starts with a `#` heading followed by a blank   |               |
|      | line.                                                |               |
| py   | Extension `.py`, or a shebang mentioning python.     | py/syntax.cpp |)"},
    {"Re-wrap after editing an already-wrapped cell", 1,
     R"(| Option | Description |
|--------|---------------------------------------------------------------------|
| `-n`   | Do not create the file if it does not exist yet; instead report an error and exit with status 2 |
|        | so that scripts can tell the two cases apart.                       |
| `-v`   | Verbose.                                                            |)",
     R"(| Option | Description                                                         |
| ------ | ------------------------------------------------------------------- |
| `-n`   | Do not create the file if it does not exist yet; instead report an  |
|        | error and exit with status 2 so that scripts can tell the two cases |
|        | apart.                                                              |
| `-v`   | Verbose.                                                            |)"},
    {"Unwrap when the content fits again", 1,
     R"(| Flag | Meaning |
|------|----------------------------------------|
| `-q` | Quiet mode: suppress   |
|      | all output.                            |)",
     R"(| Flag | Meaning                          |
| ---- | -------------------------------- |
| `-q` | Quiet mode: suppress all output. |)"},
    {"Two wrapped columns in one logical row", 2,
     R"(| Case | Before | After |
|---|---|---|
| 1 | old behaviour: wraps the | The whole table is merged into a single paragraph and every pipe ends up on one very long line, which destroys the table completely. |
|   | whole table | |
| 2 | ok | ok |)",
     R"(| Case | Before                   | After                                    |
| ---- | ------------------------ | ---------------------------------------- |
| 1    | old behaviour: wraps the | The whole table is merged into a single  |
|      | whole table              | paragraph and every pipe ends up on one  |
|      |                          | very long line, which destroys the table |
|      |                          | completely.                              |
| 2    | ok                       | ok                                       |)"},
    {"Table without outer pipes", -1,
     R"(Name | Value
--- | ---:
alpha | 1
beta | 22)",
     R"(| Name  | Value |
| ----- | ----: |
| alpha |     1 |
| beta  |    22 |)"},
    {"Ragged rows and escaped pipes", -1,
     R"(| Op | Syntax |
|---|---|
| or | `a \| b` |
| not |
| xor | `a ^ b` | extra |)",
     R"(| Op  | Syntax   |       |
| --- | -------- | ----- |
| or  | `a \| b` |       |
| not |          |       |
| xor | `a ^ b`  | extra |)"},
    {"Wide characters", -1,
     R"(| Lang | Greeting |
|---|---|
| ja | こんにちは |
| en | hello |
| ko | 안녕하세요 |)",
     R"(| Lang | Greeting   |
| ---- | ---------- |
| ja   | こんにちは |
| en   | hello      |
| ko   | 안녕하세요 |)"},
    {"Atomic inline spans", 1,
     R"(| Step | Command |
|---|---|
| build | Run `cmake -B build -DCMAKE_BUILD_TYPE=Release` once, then `cmake --build build -j12` for every rebuild; see [the build docs](docs/architecture.md). |)",
     R"(| Step  | Command                                                    |
| ----- | ---------------------------------------------------------- |
| build | Run `cmake -B build -DCMAKE_BUILD_TYPE=Release` once, then |
|       | `cmake --build build -j12` for every rebuild; see          |
|       | [the build docs](docs/architecture.md).                    |)"},
    {"Unbreakable word wider than the budget", 1,
     R"(| Ref | Link |
|---|---|
| GFM tables spec | See https://github.github.com/gfm/#tables-extension-and-the-surrounding-sections-on-inlines for details. |)",
     R"(| Ref             | Link                                                                                    |
| --------------- | --------------------------------------------------------------------------------------- |
| GFM tables spec | See                                                                                     |
|                 | https://github.github.com/gfm/#tables-extension-and-the-surrounding-sections-on-inlines |
|                 | for details.                                                                            |)"},
    {"Indented table (inside a list item)", 1,
     R"(    | Run | Notes |
    |---|---|
    | a | Fast path taken for every input because the cache was warm after the first iteration of the loop. |)",
     R"(    | Run | Notes                                                            |
    | --- | ---------------------------------------------------------------- |
    | a   | Fast path taken for every input because the cache was warm after |
    |     | the first iteration of the loop.                                 |)"},
    {"Header cell is never wrapped", 1,
     R"(| Id | A rather long column heading that names the column |
|---|---|
| 1 | short |
| 2 | This body cell is long enough that it has to wrap onto more than one physical line at eighty. |)",
     R"(| Id  | A rather long column heading that names the column                   |
| --- | -------------------------------------------------------------------- |
| 1   | short                                                                |
| 2   | This body cell is long enough that it has to wrap onto more than one |
|     | physical line at eighty.                                             |)"},
    {"Other columns already exceed 80", 2,
     R"(| Name | Path | Description |
|---|---|---|
| core | src/buffer/modes/md/really/deeply/nested/directory/structure/table.cpp | Table reflow logic and alignment. |)",
     R"(| Name | Path                                                                   | Description        |
| ---- | ---------------------------------------------------------------------- | ------------------ |
| core | src/buffer/modes/md/really/deeply/nested/directory/structure/table.cpp | Table reflow logic |
|      |                                                                        | and alignment.     |)"},
    {"Limitation: an empty first cell means continuation", 2,
     R"(| Group | Member | Notes |
|---|---|---|
| A | x | first |
| | y | second |
| | z | third |)",
     R"(| Group | Member | Notes              |
| ----- | ------ | ------------------ |
| A     | x      | first second third |
|       | y      |                    |
|       | z      |                    |)"},
};

} // namespace

BOOST_AUTO_TEST_CASE(examples) {
  for (auto const& ex : kExamples) {
    BOOST_TEST_CONTEXT(ex.name) { BOOST_CHECK_EQUAL(format(ex.input, ex.column), ex.expected); }
  }
}

BOOST_AUTO_TEST_CASE(formatting_is_idempotent) {
  for (auto const& ex : kExamples) {
    BOOST_TEST_CONTEXT(ex.name) { BOOST_CHECK_EQUAL(format(ex.expected, ex.column), ex.expected); }
  }
}

// Found by func_fuzz: a backtick left open on one line of a wrapped cell
// and a code span on the next were split into words line by line, then
// joined. The second run paired the backticks differently, so the
// spaces inside the old code span were collapsed.
BOOST_AUTO_TEST_CASE(backtick_pairing_across_wrapped_lines_is_stable) {
  auto once = format("| k | v |\n|---|---|\n| a | ` |\n|   | `y  z` |", 1);
  BOOST_CHECK_EQUAL(once, "| k   | v       |\n"
                          "| --- | ------- |\n"
                          "| a   | ` `y z` |");
  BOOST_CHECK_EQUAL(format(once, 1), once);
}

BOOST_AUTO_TEST_CASE(emoji_count_as_two_columns) {
  BOOST_CHECK_EQUAL(format("| Lang | Greeting |\n|---|---|\n| emoji | 👋🙂 |\n| en | hello |", -1),
                    "| Lang  | Greeting |\n"
                    "| ----- | -------- |\n"
                    "| emoji | 👋🙂     |\n"
                    "| en    | hello    |");
}

BOOST_AUTO_TEST_CASE(rejects_non_tables) {
  std::vector<std::string> noDelimiter{"| a | b |", "| c | d |"};
  BOOST_CHECK(!formatMarkdownTable(noDelimiter, std::nullopt, 80));
  std::vector<std::string> oneLine{"| a | b |"};
  BOOST_CHECK(!formatMarkdownTable(oneLine, std::nullopt, 80));
}

BOOST_AUTO_TEST_CASE(find_table_bounds) {
  auto lines = toLineStore({
      "Prose with a | pipe", // 0: directly above, not part of the table
      "| a | b |",           // 1: header
      "|---|---|",           // 2
      "| 1 | 2 |",           // 3
      "",                    // 4
      "| x | y |",           // 5: no delimiter row
      "| z | w |",           // 6
      "| p | q |",           // 7: delimiter cell count differs
      "|---|",               // 8
      "plain text",          // 9
  });
  for (std::size_t l : {1u, 2u, 3u}) {
    auto t = findMarkdownTable(lines, l);
    BOOST_REQUIRE(t);
    BOOST_CHECK_EQUAL(t->first, 1u);
    BOOST_CHECK_EQUAL(t->last, 3u);
  }
  for (std::size_t l : {0u, 4u, 5u, 6u, 7u, 8u, 9u})
    BOOST_CHECK(!findMarkdownTable(lines, l));
}

// The tokenizer's line-by-line scan must find the same tables as Alt-q
// does, except that it only knows a header once it is past it.
BOOST_AUTO_TEST_CASE(line_scan_agrees_with_find_table) {
  std::vector<std::vector<std::string>> docs{
      {"Prose with a | pipe", "| a | b |", "|---|---|", "| 1 | 2 |", "", "| x | y |", "| z | w |",
       "| p | q |", "|---|", "plain text"},
      // A delimiter row with the wrong cell count, then a real table.
      {"a | b | c", "|---|", "| x |", "|---|", "| y |", "text", "|---|", "| z |"},
      {"> | a | b |", "> |---|---|", "> | long cell | 2 |", "text"},
      {"\xef\xbb\xbf| a | b |", "|---|---|", "| 1 | 2 |"},
      {"a | b", "--- | ---", "1 | 2", "3", "4 | 5"},
      {"|", "|---|", "| a |", "|---|"},
  };
  for (auto const& doc : docs) {
    auto lines = toLineStore(doc);
    MdTableScan scan;
    for (std::size_t l = 0; l < doc.size(); ++l) {
      auto table = findMarkdownTable(lines, l);
      bool const belowHeader = table && l > table->first;
      BOOST_CHECK_MESSAGE(scan.advance(doc[l]) == belowHeader, "line " << l << ": " << doc[l]);
    }
  }
}

BOOST_AUTO_TEST_CASE(cursor_follows_text_through_rewrap) {
  // Cursor just before "jumps" in a cell that gets wrapped.
  std::vector<std::string> lines{
      "| k | v |",
      "|---|---|",
      "| a | the quick brown fox jumps over the lazy dog and keeps on running for a while longer |",
  };
  auto off = lines[2].find("jumps");
  auto result = formatMarkdownTable(lines, BufferPoint{2, off}, 40);
  BOOST_REQUIRE(result && result->cursor);
  auto const& line = result->lines[result->cursor->line];
  // Placed after the same non-blank text, i.e. right after "fox".
  BOOST_CHECK_EQUAL(line.substr(result->cursor->offset - 3, 3), "fox");
}

BOOST_AUTO_TEST_CASE(cursor_in_continuation_row) {
  std::vector<std::string> lines{
      "| k | v           |",
      "|---|-------------|",
      "| a | alpha beta  |",
      "|   | gamma delta |",
  };
  auto off = lines[3].find("delta");
  auto result = formatMarkdownTable(lines, BufferPoint{3, off}, 80);
  BOOST_REQUIRE(result && result->cursor);
  BOOST_CHECK_EQUAL(joinLines(result->lines), "| k   | v                      |\n"
                                              "| --- | ---------------------- |\n"
                                              "| a   | alpha beta gamma delta |");
  BOOST_CHECK_EQUAL(result->cursor->line, 2u);
  BOOST_CHECK_EQUAL(result->cursor->offset, result->lines[2].find("delta") - 1);
}

BOOST_AUTO_TEST_CASE(cursor_on_delimiter_row_stays_there) {
  std::vector<std::string> lines{"| k | v |", "|:-|--:|", "| a | b |"};
  auto result = formatMarkdownTable(lines, BufferPoint{1, 5}, 80);
  BOOST_REQUIRE(result && result->cursor);
  BOOST_CHECK_EQUAL(result->cursor->line, 1u);
  BOOST_CHECK_EQUAL(result->lines[1].substr(result->cursor->offset), "--: |");
}

// --- BufferView integration ---

namespace {

constexpr char const* kDoc = "Some intro text\n"
                             "on two lines.\n"
                             "| k | v |\n"
                             "|---|---|\n"
                             "| a | b |\n"
                             "\n"
                             "Another\n"
                             "paragraph.\n";

} // namespace

BOOST_AUTO_TEST_CASE(view_formats_table_under_cursor_as_one_undo_step) {
  TempFile f(kDoc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  auto original = contents(buf);
  view.setPoint(4, 6); // on "b"
  auto before = buf.versionHistoryInfo().size;
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(contents(buf), "Some intro text\n"
                                   "on two lines.\n"
                                   "| k   | v   |\n"
                                   "| --- | --- |\n"
                                   "| a   | b   |\n"
                                   "\n"
                                   "Another\n"
                                   "paragraph.\n");
  BOOST_CHECK_EQUAL(view.point().line, 4u);
  BOOST_CHECK_EQUAL(view.point().offset, 8u);
  BOOST_CHECK_EQUAL(buf.versionHistoryInfo().size, before + 1);

  BufferPoint cursor;
  buf.applyVersionAt(before - 1, cursor);
  BOOST_CHECK_EQUAL(contents(buf), original);
}

BOOST_AUTO_TEST_CASE(view_paragraph_reflow_stops_at_adjacent_table) {
  TempFile f(kDoc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(contents(buf), "Some intro text on two lines.\n"
                                   "| k | v |\n"
                                   "|---|---|\n"
                                   "| a | b |\n"
                                   "\n"
                                   "Another\n"
                                   "paragraph.\n");
}

BOOST_AUTO_TEST_CASE(view_region_formats_touched_tables_entirely) {
  TempFile f(kDoc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(0, 0);
  view.setMark({0, 0});
  view.setPoint(3, 1); // region ends inside the table
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(contents(buf), "Some intro text on two lines.\n"
                                   "| k   | v   |\n"
                                   "| --- | --- |\n"
                                   "| a   | b   |\n"
                                   "\n"
                                   "Another\n"
                                   "paragraph.\n");
}

BOOST_AUTO_TEST_CASE(view_region_keeps_blank_lines) {
  TempFile f(kDoc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setMark({0, 0});
  view.setPoint(8, 0);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(contents(buf), "Some intro text on two lines.\n"
                                   "| k   | v   |\n"
                                   "| --- | --- |\n"
                                   "| a   | b   |\n"
                                   "\n"
                                   "Another paragraph.\n");
}

BOOST_AUTO_TEST_CASE(view_ignores_tables_outside_markdown) {
  TempFile f(kDoc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(4, 0);
  view.reflowParagraph(80);
  BOOST_CHECK_EQUAL(buf.lineContent(0),
                    "Some intro text on two lines. | k | v | |---|---| | a | b |");
}

BOOST_AUTO_TEST_CASE(view_formatted_table_leaves_buffer_unmodified) {
  TempFile f("| k   | v   |\n| --- | --- |\n| a   | b   |\n");
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(2, 2);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK(!buf.modified());
  BOOST_CHECK_EQUAL(view.point().line, 2u);
  BOOST_CHECK_EQUAL(view.point().offset, 2u);
}

// --- Prefixes, spacer rows, awkward cells ---

BOOST_AUTO_TEST_CASE(block_quoted_table) {
  auto lines = toLineStore({"> | a | b |", "> |---|---|", "> | long cell | 2 |", "text"});
  auto t = findMarkdownTable(lines, 2);
  BOOST_REQUIRE(t);
  BOOST_CHECK_EQUAL(t->first, 0u);
  BOOST_CHECK_EQUAL(t->last, 2u);
  BOOST_CHECK_EQUAL(format("> | a | b |\n> |---|---|\n>| long cell | 2 |", -1),
                    "> | a         | b   |\n"
                    "> | --------- | --- |\n"
                    "> | long cell | 2   |");
  // Nested quote, continuation rows get the prefix too, and it counts
  // toward the width.
  auto out = format("> > | k | v |\n> > |---|---|\n> > | a | one two three four five six seven "
                    "eight nine ten eleven twelve thirteen fourteen fifteen sixteen |",
                    1);
  for (auto const& l : splitLines(out)) {
    BOOST_CHECK(l.starts_with("> > | "));
    BOOST_CHECK_LE(visualWidth(l), 80u);
  }
  BOOST_CHECK_GE(splitLines(out).size(), 4u);
}

BOOST_AUTO_TEST_CASE(table_after_byte_order_mark) {
  std::vector<std::string> lines{"\xef\xbb\xbf| a | b |", "|---|---|", "| 1 | 22 |"};
  BOOST_CHECK(findMarkdownTable(toLineStore(lines), 0));
  auto r = formatMarkdownTable(lines, BufferPoint{2, 2}, 80);
  BOOST_REQUIRE(r);
  BOOST_CHECK_EQUAL(joinLines(r->lines), "\xef\xbb\xbf| a   | b   |\n"
                                         "| --- | --- |\n"
                                         "| 1   | 22  |");
  // Only a header carries a BOM as prefix; elsewhere it is cell text.
  std::vector<std::string> body{"| a | b |", "|---|---|", "\xef\xbb\xbf| 1 | 2 |"};
  auto r2 = formatMarkdownTable(body, std::nullopt, 80);
  BOOST_REQUIRE(r2);
  BOOST_CHECK(joinLines(r2->lines).find("\xef\xbb\xbf") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(spacer_rows_are_kept) {
  std::string table = "| k   | v   |\n"
                      "| --- | --- |\n"
                      "| a   | 1   |\n"
                      "|     |     |\n"
                      "|     | 2   |\n"
                      "| b   | 3   |";
  BOOST_CHECK_EQUAL(format(table, -1), table);
  // Wrapping column v doesn't pull "2" up into row a across the spacer.
  BOOST_CHECK_EQUAL(format(table, 1), table);
}

BOOST_AUTO_TEST_CASE(cell_starting_with_combining_mark_stays_aligned) {
  std::vector<std::string> lines{"| a | b |", "|---|---|", "| \xcc\x81x | y |", "| zz | w |"};
  auto off = lines[2].find("\xcc\x81");
  auto r = formatMarkdownTable(lines, BufferPoint{2, off}, 80);
  BOOST_REQUIRE(r && r->cursor);
  for (auto const& l : r->lines)
    BOOST_CHECK_EQUAL(visualWidth(l), visualWidth(r->lines[0]));
  // The mark joins the padding space, so the cursor sits before that space.
  auto const& line = r->lines[r->cursor->line];
  BOOST_CHECK_EQUAL(clusterStart(line, r->cursor->offset), r->cursor->offset);
  BOOST_CHECK(std::string_view(line).substr(r->cursor->offset).starts_with(" \xcc\x81x"));
}

BOOST_AUTO_TEST_CASE(cell_ending_in_backslash_blank_is_stable) {
  std::vector<std::string> lines{"| k | v |", "|---|---|", "| a | word\\ \t |", "| b | x\\ y |"};
  auto r1 = formatMarkdownTable(lines, BufferPoint{2, 6}, 80);
  BOOST_REQUIRE(r1);
  auto r2 = formatMarkdownTable(r1->lines, r1->cursor, 80);
  BOOST_REQUIRE(r2);
  BOOST_CHECK(r1->lines == r2->lines);
  BOOST_CHECK_EQUAL(r1->lines[2], "| a   | word\\ |");
}

// --- Fenced code blocks ---

BOOST_AUTO_TEST_CASE(fenced_lines) {
  auto lines = toLineStore({
      "text",    // 0
      "```sh",   // 1 open
      "a | b",   // 2
      "~~~",     // 3 other fence char: content
      "``",      // 4 too short to close
      "````",    // 5 close (longer is fine)
      "text",    // 6
      "  ~~~~",  // 7 open, indented
      "~~~",     // 8 shorter: content
      "~~~~ x",  // 9 trailing text: content
      "~~~~~  ", // 10 close
      "``` a`b", // 11 backtick in info string: not a fence
      "```",     // 12 open, never closed
      "| a |",   // 13
  });
  std::vector<bool> expected{false, true, true, true, true,  true, false,
                             true,  true, true, true, false, true, true};
  BOOST_CHECK(markdownFencedLines(lines) == expected);
}

BOOST_AUTO_TEST_CASE(view_leaves_fenced_code_alone) {
  std::string doc = "intro\n"
                    "text\n"
                    "```\n"
                    "$ ls | wc\n"
                    "-- | --\n"
                    "foo | bar\n"
                    "long   spaced   line\n"
                    "```\n"
                    "after\n"
                    "text\n";
  {
    TempFile f(doc);
    TextBuffer buf(f.path().string(), f.path());
    BufferView view(buf);
    view.setPoint(5, 0); // inside the fence: no-op
    view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
    BOOST_CHECK_EQUAL(contents(buf), doc);
    BOOST_CHECK(!buf.modified());
  }
  {
    TempFile f(doc);
    TextBuffer buf(f.path().string(), f.path());
    BufferView view(buf);
    view.setMark({0, 0}); // region over everything: only the prose reflows
    view.setPoint(10, 0);
    view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
    BOOST_CHECK_EQUAL(contents(buf), "intro text\n"
                                     "```\n"
                                     "$ ls | wc\n"
                                     "-- | --\n"
                                     "foo | bar\n"
                                     "long   spaced   line\n"
                                     "```\n"
                                     "after text\n");
  }
}

BOOST_AUTO_TEST_CASE(long_pipe_run_without_table_is_linear) {
  // 20,000 lines with a pipe but no delimiter row. Scanning the run once
  // per line took about 40 s here; once per run takes milliseconds.
  std::string doc = "intro\ntext\n";
  for (int i = 0; i < 20000; ++i)
    doc += "a | b | c\n";
  TempFile f(doc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  auto start = std::chrono::steady_clock::now();
  view.setPoint(0, 0);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown); // paragraph next to the run
  view.setMark({0, 0});
  view.setPoint(20002, 0);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown); // region over the run
  BOOST_CHECK_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
  // No delimiter row, so the run is prose and joins the paragraph.
  BOOST_CHECK(buf.lineContent(0).starts_with("intro text a | b | c a | b"));
}

// --- Only the changed span is rewritten ---

BOOST_AUTO_TEST_CASE(view_rewrites_only_changed_lines) {
  std::string doc = "| k   | v   |\n"
                    "| --- | --- |\n"
                    "| a   | 1   |\n"
                    "| b | 2 |\n" // the only misaligned row
                    "| c   | 3   |\n";
  TempFile f(doc);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  LineStore before = buf.lines(); // a snapshot: shares chunks and lines
  view.setPoint(0, 2);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(buf.lineContent(3), "| b   | 2   |");
  for (std::size_t i : {0u, 1u, 2u, 4u})
    BOOST_CHECK(buf.lines()[i] == before[i]); // same storage, not just same text
  BOOST_CHECK(buf.lines()[3] != before[3]);
  // One changed line, same line count: undo still restores it.
  BufferPoint cursor;
  buf.applyVersionAt(buf.versionHistoryInfo().size - 2, cursor);
  BOOST_CHECK_EQUAL(contents(buf), doc);
}

BOOST_AUTO_TEST_CASE(view_rewrite_that_only_adds_or_removes_lines) {
  // Wrapping adds continuation rows after an unchanged row ...
  std::string wide =
      "| k   | v                                                                        |\n"
      "| --- | ------------------------------------------------------------------------ |\n"
      "| a   | one two three four five six seven eight nine ten eleven twelve thirteen fourteen "
      "fifteen sixteen |\n";
  TempFile f(wide);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);
  view.setPoint(2, 8);
  view.reflowParagraph(80, BufferView::ReflowTables::Markdown);
  BOOST_CHECK_EQUAL(buf.lineCount(), 5u); // header, delimiter, 2 rows, trailing empty line
  for (std::size_t i = 0; i + 1 < buf.lineCount(); ++i)
    BOOST_CHECK_LE(visualWidth(buf.lineContent(i)), 80u);
  // ... and a paragraph whose tail joins the previous line only removes one.
  TempFile g("aaa bbb\nccc\n\nkeep\n");
  TextBuffer buf2(g.path().string(), g.path());
  BufferView view2(buf2);
  view2.setPoint(0, 0);
  view2.reflowParagraph(80);
  BOOST_CHECK_EQUAL(contents(buf2), "aaa bbb ccc\n\nkeep\n");
  BOOST_CHECK_EQUAL(view2.point().line, 0u);
  BOOST_CHECK_EQUAL(view2.point().offset, 11u);
  // Region reflow where only blank-separated paragraphs in the middle change.
  TempFile h("same\n\nx\ny\n\nsame\n");
  TextBuffer buf3(h.path().string(), h.path());
  BufferView view3(buf3);
  view3.setMark({0, 0});
  view3.setPoint(6, 0);
  view3.reflowParagraph(80);
  BOOST_CHECK_EQUAL(contents(buf3), "same\n\nx y\n\nsame\n");
  BOOST_CHECK_EQUAL(view3.point().line, 4u);
  BOOST_CHECK_EQUAL(view3.point().offset, 4u);
  BufferPoint cursor;
  buf3.applyVersionAt(buf3.versionHistoryInfo().size - 1, cursor);
  BOOST_CHECK_EQUAL(contents(buf3), "same\n\nx y\n\nsame\n");
}
