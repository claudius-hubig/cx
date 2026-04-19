#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single markdown line, writing per-byte SyntaxToken values
// into `out`. `out` is resized/overwritten; reusing a vector avoids
// heap allocation per line. state is unused for markdown (no multi-line
// constructs) but kept for interface consistency with the C tokenizer.
void tokenizeMarkdownLine(std::string const& line, SyntaxState& state,
                          std::vector<SyntaxToken>& out);
