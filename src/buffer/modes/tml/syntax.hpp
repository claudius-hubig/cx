#pragma once

/**
 * @file tml/syntax.hpp
 * @brief TOML tokenizer (table headers, keys, strings, literal values).
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

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
 * nested arrays on continuation lines are not mistaken for headers. No
 * other SyntaxState field is touched, matching the "one mode per buffer"
 * invariant in c/syntax.hpp.
 */
void tokenizeTomlLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
