#pragma once

/**
 * @file py/syntax.hpp
 * @brief Python tokenizer (strings and triple-quotes, keywords, numbers,
 *        decorators).
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/**
 * @brief Tokenize a single Python line, writing per-byte SyntaxToken values
 *        into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation.
 *
 * Multi-line state: @c state.pyTriple carries an open '''/""" string across
 * lines (it holds the quote char; @c state.pyTripleRaw suppresses
 * backslash-escape handling for r'''...''' forms). While set, lines are one
 * String span until the matching triple quote. No other SyntaxState field
 * is touched, matching the "one mode per buffer" invariant in c/syntax.hpp.
 */
void tokenizePythonLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
