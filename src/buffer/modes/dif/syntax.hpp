#pragma once

/**
 * @file dif/syntax.hpp
 * @brief Diff/patch tokenizer: classifies whole lines as add, remove,
 *        file-header, or hunk-header.
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/**
 * @brief Tokenize a single line of unified diff output.
 *
 * Classifies the entire line based on its prefix character(s). The
 * @p state parameter is unused (diff has no multi-line constructs) but
 * kept for interface consistency with the C tokenizer.
 */
void tokenizeDiffLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
