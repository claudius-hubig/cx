#pragma once

/**
 * @file tml/syntax.hpp
 * @brief TOML tokenizer (table headers, keys, strings, literal values).
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/** @brief TOML tokenizer state carried between lines. */
struct TmlSyntaxState {
  /**
   * @brief TOML multi-line string: 0 = not inside; '\'' or '"' = the open
   *        '''/""" quote character.
   */
  char tomlTriple = 0;
  /** @brief TOML `[`/`{` still open at the end of the line (multi-line arrays). */
  int tomlDepth = 0;
};

/**
 * @brief Tokenize a single TOML line, writing per-byte SyntaxToken values
 *        into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation.
 *
 * Multi-line state: @c state.tomlTriple carries an open '''/""" string
 * across lines (it holds the quote char), and @c state.tomlDepth counts
 * `[`/`{` left open by earlier lines (multi-line arrays). Keys and table
 * headers are only recognised at line start while @c tomlDepth is 0, so
 * nested arrays on continuation lines are not mistaken for headers.
 */
void tokenizeTomlLine(std::string const& line, TmlSyntaxState& state,
                      std::vector<SyntaxToken>& out);
