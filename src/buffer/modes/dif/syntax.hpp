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
 * Classifies the entire line based on its prefix character(s). Diff has no
 * multi-line constructs, so there is no state to carry between lines.
 */
void tokenizeDiffLine(std::string const& line, std::vector<SyntaxToken>& out);
