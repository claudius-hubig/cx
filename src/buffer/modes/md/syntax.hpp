#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single markdown line, producing a per-byte SyntaxToken vector.
// Recognises: headings (# ...), bold (**...**), and links ([text](url)).
// state is unused for markdown (no multi-line constructs) but kept for
// interface consistency with the C tokenizer.
std::vector<SyntaxToken> tokenizeMarkdownLine(std::string const& line, SyntaxState& state);
