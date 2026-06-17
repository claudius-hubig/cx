#pragma once

/**
 * @file md/syntax.hpp
 * @brief Markdown tokenizer: headings, bold, and (possibly multi-line)
 *        links.
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/**
 * @brief Tokenize a single Markdown line, writing per-byte SyntaxToken
 *        values into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation. @p state carries the cross-line link state (see MdLink).
 */
void tokenizeMarkdownLine(std::string const& line, SyntaxState& state,
                          std::vector<SyntaxToken>& out);
