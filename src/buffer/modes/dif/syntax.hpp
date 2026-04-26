#pragma once

#include "buffer/modes/c/syntax.hpp"

#include <string>
#include <vector>

// Tokenize a single line of unified diff output.
// Classifies the entire line based on its prefix character(s).
void tokenizeDiffLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
