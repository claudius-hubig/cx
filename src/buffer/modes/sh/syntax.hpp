#pragma once

/**
 * @file sh/syntax.hpp
 * @brief Shell tokenizer (comments, strings, heredocs, $VAR expansions,
 *        keywords, operators).
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/**
 * @brief Tokenize a single shell (sh/bash/zsh/ksh) line, writing per-byte
 *        SyntaxToken values into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation.
 *
 * Multi-line state: @c state.heredocTag carries an open heredoc across
 * lines — while set, every subsequent line is one String span until a line
 * whose content equals the tag (leading tabs stripped first for the `<<-`
 * form, tracked by @c state.heredocStripTabs). No other SyntaxState field
 * is touched, matching the "one mode per buffer" invariant in
 * c/syntax.hpp.
 */
void tokenizeShellLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
