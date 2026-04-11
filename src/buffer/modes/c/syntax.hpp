#pragma once

#include "types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
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
  Heading,  // Markdown headings (# ...)
  Bold,     // Markdown bold (**...**)
  Link,     // Markdown links [text](url)
  Number,   // JSON numeric literals
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
// Cheaper than tokenizeLine: skips identifier/keyword classification,
// operator and bracket tagging, and the per-byte token vector
// allocation. Only tracks block comment and raw-string-literal state.
void advanceBlockCommentState(std::string const& line, SyntaxState& state);

// Callable invoked by findMatchingBracket to fetch tokens for a line.
// Each line visited during the scan is queried at most once per call.
using LineTokensFn = std::function<std::span<SyntaxToken const>(std::size_t lineIdx)>;

// Find the matching bracket for the character at pos in the buffer.
// Returns nullopt if pos is not on a bracket or no match is found.
// The three-argument overload uses the caller-supplied tokens source
// (typically backed by a syntax cache). The two-argument overload
// tokenizes lines lazily and memoizes them for the call's lifetime.
std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos, LineTokensFn const& getTokens);
std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos);
