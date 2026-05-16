#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single YAML line, writing per-byte SyntaxToken values
// into `out`. `out` is resized/overwritten; reusing a vector avoids
// per-line heap allocation.
//
// Multi-line state: `state.yamlBlockCol` carries a `|`/`>` block
// scalar across lines. While >= 0, every line indented strictly
// deeper than that column (or blank) is one String span; the first
// non-blank line indented back to <= that column ends the block and
// is tokenized normally. No other SyntaxState field is touched,
// matching the "one mode per buffer" invariant in c/syntax.hpp.
void tokenizeYamlLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
