#pragma once

/**
 * @file md/syntax.hpp
 * @brief Markdown tokenizer: headings, bold, and (possibly multi-line)
 *        links, outside fenced code blocks.
 */

#include "buffer/modes/c/syntax.hpp"
#include "buffer/modes/md/fence.hpp"
#include "buffer/modes/md/table.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief State of a cross-line Markdown link.
 *
 * Tracks a `[text](url)` link that spans lines: @c Label while the `[...`
 * label is still open (no `]` yet), @c Url while the `](...` destination is
 * still open (no `)` yet). An unterminated link stays Link-coloured to the
 * end of its block, since link text cannot span blocks: a blank line, a
 * fenced code block, a list item (see listMarkerLength()) and a table row
 * (see MdTableScan) each close it. These are the places where Alt-q
 * starts a new block too. (To the end of the file, as an unterminated raw
 * string or heredoc runs, would let one unmatched `[` in prose recolour
 * everything after it.)
 *
 * A table row is a block of its own: no link is carried into or out of
 * it, so a `[` that is not closed on the row is plain text. The header
 * row is the exception, as it is not known to be one until the delimiter
 * row below it has been seen.
 */
enum class MdLink : uint8_t { None, Label, Url };

/** @brief Markdown tokenizer state carried between lines. */
struct MdSyntaxState {
  MdLink mdLink = MdLink::None; /**< Cross-line link state (see MdLink). */
  MdTableScan table;            /**< Whether the line is in a table. */
  MdFence fence;                /**< The fenced code block the line is in, if any. */

  bool operator==(MdSyntaxState const&) const = default;
};

/**
 * @brief Tokenize a single Markdown line, writing per-byte SyntaxToken
 *        values into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation. @p state carries the cross-line link state (see MdLink),
 * whether the line is in a table, and the open code fence: the lines of a
 * fenced code block, fence lines included, are not Markdown and get no
 * markup tokens.
 */
void tokenizeMarkdownLine(std::string const& line, MdSyntaxState& state,
                          std::vector<SyntaxToken>& out);
