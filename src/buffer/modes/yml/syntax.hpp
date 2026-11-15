#pragma once

/**
 * @file yml/syntax.hpp
 * @brief YAML tokenizer (keys, strings, constants, anchors/tags, block
 *        scalars).
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/** @brief YAML tokenizer state carried between lines. */
struct YmlSyntaxState {
  /**
   * @brief YAML block scalar: >= 0 = inside a `|`/`>` scalar introduced at
   *        the given indent column (deeper/blank lines are String content
   *        until indentation returns to <= this column); -1 = not in one.
   */
  int yamlBlockCol = -1;
};

/**
 * @brief Tokenize a single YAML line, writing per-byte SyntaxToken values
 *        into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation.
 *
 * Multi-line state: @c state.yamlBlockCol carries a `|`/`>` block scalar
 * across lines — while >= 0, every line indented strictly deeper than that
 * column (or blank) is one String span; the first non-blank line indented
 * back to <= that column ends the block and is tokenized normally.
 */
void tokenizeYamlLine(std::string const& line, YmlSyntaxState& state,
                      std::vector<SyntaxToken>& out);
