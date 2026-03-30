#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single JSON line, producing a per-byte SyntaxToken vector.
// Recognises: strings ("..."), keywords (true, false, null), numbers,
// and brackets ([] {}).
// state is unused for JSON (no multi-line constructs) but kept for
// interface consistency with the C tokenizer.
std::vector<SyntaxToken> tokenizeJsonLine(std::string const& line, SyntaxState& state);

// Find the matching bracket for the character at pos in a JSON buffer.
// Only matches [] and {}. Brackets inside strings are skipped.
// Returns nullopt if pos is not on a bracket or no match is found.
std::optional<BufferPoint> findMatchingBracketJson(std::vector<std::string> const& lines,
                                                   BufferPoint pos);
