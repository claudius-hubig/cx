#pragma once

/**
 * @file jsn/syntax.hpp
 * @brief JSON tokenizer (strings, numbers, value literals, brackets) plus
 *        JSON-specific bracket matching.
 */

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

/**
 * @brief Tokenize a single JSON line, writing per-byte SyntaxToken values
 *        into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation. JSON has no multi-line constructs, so there is no state to
 * carry between lines.
 */
void tokenizeJsonLine(std::string const& line, std::vector<SyntaxToken>& out);

/**
 * @brief Find the matching bracket for the character at @p pos in a JSON
 *        buffer, using a caller-supplied token source.
 *
 * Only `[]` and `{}` are matched; brackets inside strings are skipped.
 * @return The matching bracket position, or nullopt if @p pos is not on a
 *         bracket or no match is found.
 */
std::optional<BufferPoint> findMatchingBracketJson(LineStore const& lines, BufferPoint pos,
                                                   LineTokensFn const& getTokens);
/**
 * @brief Convenience overload that re-tokenizes each visited line from
 *        scratch.
 */
std::optional<BufferPoint> findMatchingBracketJson(LineStore const& lines, BufferPoint pos);
