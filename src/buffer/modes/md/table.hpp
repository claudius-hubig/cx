#pragma once

/**
 * @file md/table.hpp
 * @brief GFM (pipe) table detection, column alignment and column wrapping
 *        for Markdown buffers.
 *
 * A GFM table row is exactly one physical line, so a wrapped cell is
 * represented by *continuation rows*: a body row whose first cell is
 * empty continues the logical row above it, and the wrapped text lives in
 * the same column of those rows. Consequently the first column is never
 * wrapped, and the header (always a single line) is never wrapped. A
 * body row that is empty altogether is a spacer: it is kept, and the row
 * after it starts a new logical row.
 *
 * Rows may carry a prefix of indentation and block-quote markers (`> `),
 * and the header (the first line of a file) a byte-order mark before it.
 */

#include "buffer/line_store.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** @brief Inclusive line range [first, last] of a Markdown table. */
struct TableLines {
  std::size_t first;
  std::size_t last;
};

/**
 * @brief A maximal run [first, last] of lines that each contain an
 *        unescaped `|`, and the GFM table inside it, if any.
 *
 * The table starts at @c header — the first line of the run followed by
 * a delimiter row (`|---|:-:|`) with the same cell count — and extends
 * to @c last. Lines of the run above the header (prose that happens to
 * contain a pipe) are not part of it.
 */
struct PipeRun {
  std::size_t first;
  std::size_t last;
  std::optional<std::size_t> header;
};

/**
 * @brief The pipe run containing line @p line; std::nullopt if the line
 *        has no unescaped `|`.
 *
 * Costs one scan of the run, so callers asking about many lines should
 * reuse the result for every line in [first, last].
 * @pre @p line < @p lines.size().
 */
std::optional<PipeRun> findPipeRun(LineStore const& lines, std::size_t line);

/**
 * @brief Find the GFM table that contains line @p line: findPipeRun()
 *        narrowed to the table. Returns std::nullopt when @p line is not
 *        part of a table.
 *
 * @pre @p line < @p lines.size().
 */
std::optional<TableLines> findMarkdownTable(LineStore const& lines, std::size_t line);

/**
 * @brief Whether a line-by-line scan is inside a GFM table.
 *
 * For a caller that sees one line at a time and cannot look ahead: the
 * Markdown tokenizer, which keeps one per line in the syntax cache, hence
 * the single byte. It agrees with findPipeRun() on the delimiter row and
 * every row below it. The header is only known once the delimiter row
 * under it has been seen, so advance() returns false for it.
 */
struct MdTableScan {
  /**
   * 0: the last line had no pipe. @c kInTable: it was a table's delimiter
   * row or a row below it. Otherwise it may be a header, and this is its
   * cell count (capped, so headers of 254 cells or more count as equal).
   */
  std::uint8_t prev = 0;
  static constexpr std::uint8_t kInTable = 255;

  /**
   * @brief Step over @p line, the line after the one last stepped over.
   * @return True if @p line is a table's delimiter row or a row below it.
   */
  bool advance(std::string_view line);

  bool operator==(MdTableScan const&) const = default;
};

/** @brief Result of formatMarkdownTable(). */
struct FormattedTable {
  std::vector<std::string> lines;    /**< Replacement lines for the table. */
  std::optional<BufferPoint> cursor; /**< Cursor relative to lines[0]; set iff one was passed in. */
};

/**
 * @brief Align a GFM table and, given a cursor, wrap the cursor's column.
 *
 * Every cell is padded to its column's width (visual width, minimum 3)
 * so all separators line up; delimiter rows are resized and keep their
 * `:` alignment markers, which also decide whether cells are padded
 * left, right or centred. Outer pipes are always emitted; the header's
 * prefix (indentation, block-quote markers) is put on every line and
 * counts toward @p targetWidth; short rows get empty cells.
 *
 * With a @p cursor outside the first column, every cell of the cursor's
 * column is refilled to whatever is left of @p targetWidth after the
 * other columns, but no narrower than the column's longest word, its
 * header, or min(20, its widest cell's text). This also unwraps cells
 * whose text fits again. Code spans and `[text](url)` links are never
 * split. Continuation rows that end up empty are dropped. The returned
 * cursor stays in the same logical cell, after the same number of
 * non-blank bytes of its text.
 *
 * @p lines are exactly the table's lines, as delimited by
 * findMarkdownTable(); returns std::nullopt if they don't form a table.
 * @p cursor is relative to lines[0].
 */
std::optional<FormattedTable> formatMarkdownTable(std::span<std::string const> lines,
                                                  std::optional<BufferPoint> cursor,
                                                  std::size_t targetWidth);
