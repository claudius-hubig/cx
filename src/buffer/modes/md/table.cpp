#include "buffer/modes/md/table.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <string_view>

namespace {

constexpr std::size_t kMinColumnWidth = 3; // room for a "---" delimiter
constexpr std::size_t kMinWrapWidth = 20;  // narrowest a wrapped column gets

bool isBlank(char c) { return c == ' ' || c == '\t'; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && isBlank(s.front()))
    s.remove_prefix(1);
  while (!s.empty() && isBlank(s.back()))
    s.remove_suffix(1);
  return s;
}

std::size_t nonBlankBytes(std::string_view s) {
  return static_cast<std::size_t>(std::ranges::count_if(s, [](char c) { return !isBlank(c); }));
}

constexpr std::string_view kBom = "\xef\xbb\xbf";

/**
 * @brief Length of what precedes the row proper: indentation and
 *        block-quote markers (`> `). With @p allowBom, a byte-order mark
 *        at the very start counts too; only a header can carry one (the
 *        first line of a file), so other rows keep it as cell text.
 */
std::size_t prefixLen(std::string_view line, bool allowBom) {
  std::size_t i = allowBom && line.starts_with(kBom) ? kBom.size() : 0;
  for (;;) {
    while (i < line.size() && isBlank(line[i]))
      ++i;
    if (i == line.size() || line[i] != '>')
      return i;
    ++i;
  }
}

/**
 * @brief Width of cell text as drawn in a row, where a space always
 *        precedes it: a leading combining mark sits on that space rather
 *        than on a dotted circle of its own.
 */
std::size_t cellWidth(std::string_view text) {
  auto w = visualWidth(text);
  bool leadingMark = !text.empty() && isCombiningMark(decodeUtf8(text.data(), text.size()).first);
  return leadingMark ? w - 1 : w;
}

/** @brief Untrimmed byte range [begin, end) of one cell within a row line. */
struct CellSpan {
  std::size_t begin;
  std::size_t end;
};

/**
 * @brief Split a row on unescaped pipes (a backslash escapes the next
 *        byte), after its prefix (see prefixLen()). Optional leading and
 *        trailing pipes don't produce empty outer cells.
 */
std::vector<CellSpan> splitRow(std::string_view line, bool allowBom = false) {
  std::vector<CellSpan> spans;
  std::size_t start = prefixLen(line, allowBom);
  std::size_t const first = start; // prefixLen() skipped the blanks
  std::size_t lastPipe = std::string_view::npos;
  for (std::size_t i = start; i < line.size(); ++i) {
    if (line[i] == '\\') {
      ++i;
    } else if (line[i] == '|') {
      spans.push_back({start, i});
      start = i + 1;
      lastPipe = i;
    }
  }
  spans.push_back({start, line.size()});

  auto last = line.find_last_not_of(" \t");
  if (first < line.size() && line[first] == '|')
    spans.erase(spans.begin());
  if (last != std::string_view::npos && last == lastPipe && !spans.empty())
    spans.pop_back();
  return spans;
}

bool hasPipe(std::string_view line) {
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\')
      ++i;
    else if (line[i] == '|')
      return true;
  }
  return false;
}

std::vector<std::string> cellsOf(std::string_view line, bool allowBom = false) {
  std::vector<std::string> cells;
  for (auto [b, e] : splitRow(line, allowBom))
    cells.emplace_back(trim(line.substr(b, e - b)));
  return cells;
}

/** @brief A delimiter cell is `:?-+:?`. */
bool isDelimiterCell(std::string_view c) {
  if (c.starts_with(':'))
    c.remove_prefix(1);
  if (c.ends_with(':'))
    c.remove_suffix(1);
  return !c.empty() && c.find_first_not_of('-') == std::string_view::npos;
}

bool isDelimiterRow(std::string_view line) {
  if (!hasPipe(line))
    return false;
  auto cells = cellsOf(line);
  return !cells.empty() && std::ranges::all_of(cells, isDelimiterCell);
}

enum class Align { None, Left, Center, Right };

Align alignmentOf(std::string_view delim) {
  bool l = delim.starts_with(':');
  bool r = delim.size() > 1 && delim.ends_with(':');
  return l && r ? Align::Center : l ? Align::Left : r ? Align::Right : Align::None;
}

/**
 * @brief Append the whitespace-separated words of @p s to @p out. Code
 *        spans and `[text](url)` / `![alt](url)` links stay one word, so
 *        they're never split across rows (each row renders separately).
 */
void appendWords(std::string_view s, std::vector<std::string>& out) {
  std::string cur;
  std::size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (isBlank(c)) {
      if (!cur.empty())
        out.push_back(std::move(cur));
      cur.clear();
      ++i;
      continue;
    }
    std::size_t len = 1;
    if (c == '\\' && i + 1 < s.size() && !isBlank(s[i + 1])) {
      len = 2; // an escaped character stays with its backslash
    } else if (c == '`') {
      // A code span closes at the next backtick run of the same length.
      auto runEnd = [&](std::size_t from) {
        return std::min(s.find_first_not_of('`', from), s.size());
      };
      auto run = runEnd(i) - i;
      len = run; // unmatched: the backticks are literal
      for (auto j = s.find('`', i + run); j != std::string_view::npos; j = s.find('`', j)) {
        auto closeRun = runEnd(j) - j;
        if (closeRun == run) {
          len = j + closeRun - i;
          break;
        }
        j += closeRun;
      }
    } else if (c == '[' || (c == '!' && i + 1 < s.size() && s[i + 1] == '[')) {
      auto close = s.find(']', i);
      if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(') {
        auto paren = s.find(')', close + 2);
        if (paren != std::string_view::npos)
          len = paren + 1 - i;
      }
    }
    cur += s.substr(i, len);
    i += len;
  }
  if (!cur.empty())
    out.push_back(std::move(cur));
}

/** @brief Greedy fill of @p words into lines of at most @p width columns. */
std::vector<std::string> fill(std::vector<std::string> const& words, std::size_t width) {
  std::vector<std::string> out;
  std::string cur;
  std::size_t curWidth = 0;
  for (auto const& w : words) {
    auto ww = visualWidth(w);
    if (!cur.empty() && curWidth + 1 + ww > width) {
      out.push_back(std::move(cur));
      cur.clear();
      curWidth = 0;
    }
    if (!cur.empty()) {
      cur += ' ';
      ++curWidth;
    }
    cur += w;
    curWidth += ww;
  }
  out.push_back(std::move(cur));
  return out;
}

/** @brief Byte offset just past the @p n-th non-blank byte of @p s. */
std::size_t offsetAfterNonBlank(std::string_view s, std::size_t n) {
  for (std::size_t i = 0; i < s.size() && n > 0; ++i)
    if (!isBlank(s[i]) && --n == 0)
      return i + 1;
  return n == 0 ? 0 : s.size();
}

} // namespace

std::optional<PipeRun> findPipeRun(LineStore const& lines, std::size_t line) {
  auto pipe = [&](std::size_t i) { return hasPipe(*lines[i]); };
  if (!pipe(line))
    return std::nullopt;
  PipeRun run{line, line, std::nullopt};
  while (run.first > 0 && pipe(run.first - 1))
    --run.first;
  while (run.last + 1 < lines.size() && pipe(run.last + 1))
    ++run.last;
  for (std::size_t h = run.first; h < run.last && !run.header; ++h)
    if (isDelimiterRow(*lines[h + 1]) &&
        splitRow(*lines[h + 1]).size() == splitRow(*lines[h], true).size())
      run.header = h;
  return run;
}

bool MdTableScan::advance(std::string_view line) {
  if (!hasPipe(line)) {
    prev = 0;
    return false;
  }
  if (prev == kInTable)
    return true;
  auto capped = [](std::size_t cells) {
    return static_cast<std::uint8_t>(std::min<std::size_t>(cells, kInTable - 1));
  };
  // The header test of findPipeRun(), with the line above remembered
  // rather than the line below looked at.
  if (prev != 0 && isDelimiterRow(line) && capped(splitRow(line).size()) == prev) {
    prev = kInTable;
    return true;
  }
  prev = capped(splitRow(line, true).size());
  return false;
}

std::optional<TableLines> findMarkdownTable(LineStore const& lines, std::size_t line) {
  auto run = findPipeRun(lines, line);
  if (!run || !run->header || line < *run->header)
    return std::nullopt;
  return TableLines{*run->header, run->last};
}

std::optional<FormattedTable> formatMarkdownTable(std::span<std::string const> lines,
                                                  std::optional<BufferPoint> cursor,
                                                  std::size_t targetWidth) {
  if (lines.size() < 2 || !isDelimiterRow(lines[1]))
    return std::nullopt;

  // Every output row gets the header's prefix (indentation, block-quote
  // markers); a byte-order mark stays on the header only.
  std::string const headPrefix = lines[0].substr(0, prefixLen(lines[0], true));
  std::string const prefix = headPrefix.substr(headPrefix.starts_with(kBom) ? kBom.size() : 0);

  std::vector<std::vector<std::string>> rows;
  std::size_t ncols = 1;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    rows.push_back(cellsOf(lines[i], i == 0));
    ncols = std::max(ncols, rows.back().size());
  }
  for (auto& r : rows)
    r.resize(ncols);

  std::vector<Align> align;
  for (auto const& c : rows[1])
    align.push_back(alignmentOf(c));

  // Logical rows, each a [column][physical line] grid. Index 0 is the
  // header and index 1 the delimiter row (its text is regenerated, so it
  // stays empty here); a body row with an empty first cell continues the
  // logical row above it. A row that is empty altogether is a spacer
  // (wrapping never produces one, so it must be the user's): it stands
  // alone, and the row after it starts afresh.
  using Cell = std::vector<std::string>;
  std::vector<std::vector<Cell>> logical;
  std::vector<std::size_t> logicalOf;
  bool afterSpacer = false;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    bool spacer = std::ranges::all_of(rows[i], &std::string::empty);
    if (i > 2 && rows[i][0].empty() && !spacer && !afterSpacer) {
      for (std::size_t c = 0; c < ncols; ++c)
        logical.back()[c].push_back(rows[i][c]);
    } else {
      logical.emplace_back(ncols);
      for (std::size_t c = 0; c < ncols; ++c)
        logical.back()[c].push_back(i == 1 ? std::string{} : rows[i][c]);
    }
    afterSpacer = i > 1 && spacer;
    logicalOf.push_back(logical.size() - 1);
  }

  // The cursor as (logical row, column, non-blank bytes of the logical
  // cell's text before it), which survives refilling.
  std::size_t curRow = 0, curCol = 0, curBytes = 0;
  if (cursor) {
    auto const& line = lines[cursor->line];
    auto spans = splitRow(line, cursor->line == 0);
    while (curCol + 1 < spans.size() && spans[curCol].end < cursor->offset)
      ++curCol;
    curRow = logicalOf[cursor->line];
    if (curRow != 1 && !spans.empty()) {
      auto [b, e] = spans[curCol];
      if (cursor->offset > b)
        curBytes = nonBlankBytes(std::string_view(line).substr(b, std::min(cursor->offset, e) - b));
      for (std::size_t i = cursor->line; i > 0 && logicalOf[i - 1] == curRow; --i)
        curBytes += nonBlankBytes(rows[i - 1][curCol]);
    }
    curCol = std::min(curCol, ncols - 1);
  }

  auto columnWidth = [&](std::size_t c) {
    std::size_t w = kMinColumnWidth;
    for (auto const& lr : logical)
      for (auto const& s : lr[c])
        w = std::max(w, cellWidth(s));
    return w;
  };

  if (cursor && curCol > 0) {
    std::size_t used = visualWidth(prefix) + 1 + 3;
    for (std::size_t c = 0; c < ncols; ++c)
      if (c != curCol)
        used += columnWidth(c) + 3;
    std::size_t budget = targetWidth > used ? targetWidth - used : 0;

    std::vector<std::vector<std::string>> words(logical.size());
    std::size_t floor = std::max(kMinColumnWidth, visualWidth(logical[0][curCol][0]));
    for (std::size_t r = 2; r < logical.size(); ++r) {
      for (auto const& s : logical[r][curCol])
        appendWords(s, words[r]);
      std::size_t joined = 0;
      for (auto const& w : words[r]) {
        floor = std::max(floor, visualWidth(w));
        joined += visualWidth(w) + (joined ? 1 : 0);
      }
      floor = std::max(floor, std::min(kMinWrapWidth, joined));
    }
    for (std::size_t r = 2; r < logical.size(); ++r)
      logical[r][curCol] = fill(words[r], std::max(budget, floor));
  }

  // Back to physical rows: every column of a logical row gets the same
  // height, minus trailing lines that are empty in all columns.
  struct OutRow {
    std::vector<std::string> cells;
    std::size_t logical;
  };
  std::vector<OutRow> out;
  for (std::size_t r = 0; r < logical.size(); ++r) {
    auto& lr = logical[r];
    std::size_t h = 1;
    for (auto const& cell : lr)
      h = std::max(h, cell.size());
    auto emptyLine = [&](std::size_t i) {
      return std::ranges::all_of(
          lr, [&](Cell const& cell) { return i >= cell.size() || cell[i].empty(); });
    };
    while (h > 1 && emptyLine(h - 1))
      --h;
    for (std::size_t i = 0; i < h; ++i) {
      OutRow row{{}, r};
      for (auto const& cell : lr)
        row.cells.push_back(i < cell.size() ? cell[i] : std::string{});
      out.push_back(std::move(row));
    }
  }

  std::vector<std::size_t> widths(ncols, kMinColumnWidth);
  for (auto const& row : out)
    for (std::size_t c = 0; c < ncols; ++c)
      widths[c] = std::max(widths[c], cellWidth(row.cells[c]));

  FormattedTable result;
  struct CursorCell {
    std::size_t line;
    std::size_t start;
    std::string_view text;
  };
  std::vector<CursorCell> cursorCells;
  for (auto const& row : out) {
    std::string line = (result.lines.empty() ? headPrefix : prefix) + "|";
    for (std::size_t c = 0; c < ncols; ++c) {
      auto w = widths[c];
      std::string text = row.cells[c];
      std::size_t textWidth = w; // a delimiter cell fills its column
      std::size_t left = 0;
      if (row.logical == 1) {
        text = align[c] == Align::Left     ? ":" + std::string(w - 1, '-')
               : align[c] == Align::Right  ? std::string(w - 1, '-') + ":"
               : align[c] == Align::Center ? ":" + std::string(w - 2, '-') + ":"
                                           : std::string(w, '-');
      } else {
        textWidth = cellWidth(text);
        auto gap = w - textWidth;
        left = align[c] == Align::Right ? gap : align[c] == Align::Center ? gap / 2 : 0;
      }
      line += ' ';
      line.append(left, ' ');
      if (cursor && row.logical == curRow && c == curCol)
        cursorCells.push_back({result.lines.size(), line.size(), row.cells[c]});
      line += text;
      line.append(w - textWidth - left, ' ');
      line += " |";
    }
    result.lines.push_back(std::move(line));
  }

  if (cursor) {
    auto remaining = curBytes;
    for (std::size_t i = 0; i < cursorCells.size(); ++i) {
      auto const& cc = cursorCells[i];
      auto n = nonBlankBytes(cc.text);
      if (remaining <= n || i + 1 == cursorCells.size()) {
        // Snapped, since a leading mark in the cell joins the space before it.
        auto const& line = result.lines[cc.line];
        result.cursor = BufferPoint{
            cc.line, clusterStart(line, cc.start + offsetAfterNonBlank(cc.text, remaining))};
        break;
      }
      remaining -= n;
    }
  }
  return result;
}
