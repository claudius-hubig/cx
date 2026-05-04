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
  String,         // "...", '...', R"(...)"
  Operator,       // +, -, *, =, &&, ||, etc.
  Paren,          // ( )
  Bracket,        // [ ]
  Brace,          // { }
  Angle,          // < > (when used as template brackets)
  Heading,        // Markdown headings (# ...)
  Bold,           // Markdown bold (**...**)
  Link,           // Markdown links [text](url)
  Number,         // JSON numeric literals
  Preprocessor,   // C/C++ preprocessor directives (#include, #define, etc.)
  DiffAdd,        // + lines in unified diff
  DiffRemove,     // - lines in unified diff
  DiffMeta,       // +++ / --- file headers in unified diff
  DiffHunk,       // @@ hunk headers in unified diff
  ConflictMarker, // <<<<<<<, =======, >>>>>>> merge conflict markers
};

/// Multi-line state carried between lines for syntax highlighting.
///
/// Invariant: at the start of any line at most one of
/// {inBlockComment, rawDelim.has_value(), inPreprocessor} is true.
/// `tokenizeLine` and `advanceBlockCommentState` both gate
/// `inPreprocessor` evaluation behind `!inBlockComment &&
/// !rawDelim.has_value()`, and the raw-string and block-comment
/// branches are mutually exclusive entry/exit points.
struct SyntaxState {
  bool inBlockComment = false;
  bool inPreprocessor = false; // next line continues a #directive (prior line ended with '\')
  std::optional<std::string> rawDelim; // has_value() = inside R"delim(...)delim"
};

// Tokenize a single line, writing per-byte SyntaxToken values into `out`.
// `out` is resized/overwritten; passing in a previously-used vector lets the
// caller reuse heap allocations. state is the multi-line state entering this
// line; updated on return to reflect state after this line.
void tokenizeLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);

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
//
// IMPORTANT: the no-callback overload always tokenizes from line 0 to
// the target line on first access (to maintain the cross-line
// SyntaxState chain), so its cost is O(file size) — not just the
// scan distance. It's intended for tests and ad-hoc use; production
// hot paths should always thread `cachedLineTokens()` through the
// callback overload instead.
std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos, LineTokensFn const& getTokens);
std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos);
