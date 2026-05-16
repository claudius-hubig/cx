#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single Python line, writing per-byte SyntaxToken values
// into `out`. `out` is resized/overwritten; reusing a vector avoids
// per-line heap allocation.
//
// Multi-line state: `state.pyTriple` carries an open '''/""" string
// across lines (it holds the quote char; `state.pyTripleRaw`
// suppresses backslash-escape handling for r'''...''' forms). While
// set, lines are one String span until the matching triple quote.
// No other SyntaxState field is touched, matching the "one mode per
// buffer" invariant documented in c/syntax.hpp.
void tokenizePythonLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
