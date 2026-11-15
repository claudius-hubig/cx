#pragma once

/**
 * @file md/syntax.hpp
 * @brief Markdown tokenizer: headings, bold, and (possibly multi-line)
 *        links.
 */

#include "buffer/modes/c/syntax.hpp"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief State of a cross-line Markdown link.
 *
 * Tracks a `[text](url)` link that spans lines: @c Label while the `[...`
 * label is still open (no `]` yet), @c Url while the `](...` destination is
 * still open (no `)` yet). An unterminated link stays Link-coloured to the
 * end of its paragraph: a blank line closes it, since link text cannot
 * span paragraphs. (To the end of the file, as an unterminated raw string
 * or heredoc runs, would let one unmatched `[` in prose recolour
 * everything after it.)
 */
enum class MdLink : uint8_t { None, Label, Url };

/** @brief Markdown tokenizer state carried between lines. */
struct MdSyntaxState {
  MdLink mdLink = MdLink::None; /**< Cross-line link state (see MdLink). */
};

/**
 * @brief Tokenize a single Markdown line, writing per-byte SyntaxToken
 *        values into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation. @p state carries the cross-line link state (see MdLink).
 */
void tokenizeMarkdownLine(std::string const& line, MdSyntaxState& state,
                          std::vector<SyntaxToken>& out);
