#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single JSON line, writing per-byte SyntaxToken values
// into `out`. `out` is resized/overwritten; reusing a vector avoids
// heap allocation per line. state is unused for JSON (no multi-line
// constructs) but kept for interface consistency with the C tokenizer.
void tokenizeJsonLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);

// Find the matching bracket for the character at pos in a JSON buffer.
// Only matches [] and {}. Brackets inside strings are skipped.
// Returns nullopt if pos is not on a bracket or no match is found.
//
// The three-argument overload uses the caller-supplied tokens source
// (typically backed by Buffer::cachedLineTokens). The two-argument
// overload re-tokenizes each visited line from scratch.
std::optional<BufferPoint> findMatchingBracketJson(std::vector<LinePtr> const& lines,
                                                   BufferPoint pos, LineTokensFn const& getTokens);
std::optional<BufferPoint> findMatchingBracketJson(std::vector<LinePtr> const& lines,
                                                   BufferPoint pos);
