#pragma once

#include "types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class SyntaxToken : uint8_t {
  Normal,
  Keyword,
  Comment,
  String,   // "...", '...', R"(...)"
  Operator, // +, -, *, =, &&, ||, etc.
  Paren,    // ( )
  Bracket,  // [ ]
  Brace,    // { }
  Angle,    // < > (when used as template brackets)
};

/// Multi-line state carried between lines for syntax highlighting.
struct SyntaxState {
  bool inBlockComment = false;
  std::optional<std::string> rawDelim; // has_value() = inside R"delim(...)delim"
};

// Tokenize a single line, producing a per-byte SyntaxToken vector.
// state is the multi-line state entering this line; updated on return
// to reflect state after this line.
std::vector<SyntaxToken> tokenizeLine(std::string const& line, SyntaxState& state);

// Advance multi-line state across a line without producing tokens.
// Cheaper than tokenizeLine when only the state is needed.
void advanceBlockCommentState(std::string const& line, SyntaxState& state);

// Find the matching bracket for the character at pos in the buffer.
// Returns nullopt if pos is not on a bracket or no match is found.
std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos);
