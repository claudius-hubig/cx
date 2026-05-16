#pragma once

#include "buffer/modes/c/syntax.hpp" // SyntaxToken, SyntaxState

#include <string>
#include <vector>

// Tokenize a single shell (sh/bash/zsh/ksh) line, writing per-byte
// SyntaxToken values into `out`. `out` is resized/overwritten; reusing
// a vector avoids per-line heap allocation.
//
// Multi-line state: `state.heredocTag` carries an open heredoc across
// lines. While set, every subsequent line is one String span until a
// line whose content equals the tag (leading tabs stripped first for
// the `<<-` form, tracked by `state.heredocStripTabs`). No other
// SyntaxState field is touched, matching the "one mode per buffer"
// invariant documented in c/syntax.hpp.
void tokenizeShellLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);
