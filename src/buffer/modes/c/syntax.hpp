#pragma once

/**
 * @file c/syntax.hpp
 * @brief The C/C++ tokenizer plus the shared syntax vocabulary used by
 *        every mode: the SyntaxToken enum and bracket matching.
 *
 * Each mode's tokenizer writes into the same SyntaxToken set, and threads
 * its own cross-line state type (here CSyntaxState) across lines; the
 * buffer's per-line syntax cache stores the one of its mode (see syntax_cache.hpp).
 */

#include "buffer/line_store.hpp"
#include "types.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** @brief Token classes shared across all mode tokenizers. */
enum class SyntaxToken : uint8_t {
  Normal,
  Keyword,
  Comment,
  String,         /**< "...", '...', R"(...)" */
  Operator,       /**< +, -, *, =, &&, ||, etc. */
  Paren,          /**< ( ) */
  Bracket,        /**< [ ] */
  Brace,          /**< { } */
  Angle,          /**< < > (when used as template brackets) */
  Heading,        /**< Markdown headings (# ...) */
  Bold,           /**< Markdown bold (**...**) */
  Link,           /**< Markdown links [text](url) */
  Number,         /**< Numeric literals (C/C++, JSON, Python, YAML) */
  Preprocessor,   /**< C/C++ preprocessor directives (\#include, \#define, etc.) */
  DiffAdd,        /**< + lines in unified diff */
  DiffRemove,     /**< - lines in unified diff */
  DiffMeta,       /**< +++ / --- file headers in unified diff */
  DiffHunk,       /**< @@ hunk headers in unified diff */
  ConflictMarker, /**< <<<<<<<, =======, >>>>>>> merge conflict markers */
  Variable,       /**< Shell $VAR / ${...} parameter expansions */
};

/**
 * @brief The delimiter of a C++ raw string literal, held inline.
 *
 * The standard allows at most 16 characters ([lex.string]), and the
 * tokenizer does not take a longer one as a raw string, so 17 bytes cover
 * every delimiter without a heap allocation or a pointer per cached line.
 */
class RawDelimiter {
public:
  static constexpr std::size_t kMaxSize = 16;

  /** @pre @p d.size() <= kMaxSize. */
  explicit RawDelimiter(std::string_view d) : _size(static_cast<std::uint8_t>(d.size())) {
    assert(d.size() <= kMaxSize);
    std::ranges::copy(d.substr(0, kMaxSize), _chars.begin());
  }
  std::string_view view() const { return {_chars.data(), _size}; }

private:
  std::array<char, kMaxSize> _chars{};
  std::uint8_t _size;
};

/**
 * @brief C/C++ tokenizer state carried between lines.
 *
 * Invariant: at the start of any line at most one of {inBlockComment,
 * rawDelim.has_value(), inPreprocessor} is true.
 */
struct CSyntaxState {
  bool inBlockComment = false;
  bool inPreprocessor = false; /**< Next line continues a \#directive (prior ended with '\'). */
  std::optional<RawDelimiter> rawDelim; /**< has_value() = inside R"delim(...)delim". */
};

/**
 * @brief Tokenize a single line, writing per-byte SyntaxToken values into
 *        @p out.
 *
 * @p out is resized/overwritten; reusing a vector lets the caller reuse
 * heap allocations. @p state is the multi-line state entering this line and
 * is updated on return to reflect the state after it.
 */
void tokenizeLine(std::string const& line, CSyntaxState& state, std::vector<SyntaxToken>& out);

/**
 * @brief Advance multi-line state across a line without producing tokens.
 *
 * Cheaper than tokenizeLine(): skips identifier/keyword classification,
 * operator/bracket tagging, and the per-byte token vector allocation. Only
 * tracks block-comment and raw-string-literal state.
 */
void advanceBlockCommentState(std::string const& line, CSyntaxState& state);

/**
 * @brief Callable invoked by findMatchingBracket() to fetch tokens for a
 *        line.
 *
 * Each line visited during the scan is queried at most once per call.
 */
using LineTokensFn = std::function<std::span<SyntaxToken const>(std::size_t lineIdx)>;

/**
 * @brief Find the matching bracket for the character at @p pos, using a
 *        caller-supplied token source (typically a syntax cache).
 * @return The matching bracket position, or nullopt if @p pos is not on a
 *         bracket or no match is found.
 */
std::optional<BufferPoint> findMatchingBracket(LineStore const& lines, BufferPoint pos,
                                               LineTokensFn const& getTokens);
/**
 * @brief Convenience overload that tokenizes lines lazily and memoizes them
 *        for the call's lifetime.
 *
 * IMPORTANT: this overload always tokenizes from line 0 to the target line
 * on first access (to maintain the cross-line CSyntaxState chain), so its
 * cost is O(file size) — not just the scan distance. Intended for tests and
 * ad-hoc use; production hot paths should thread cachedLineTokens() through
 * the callback overload instead.
 */
std::optional<BufferPoint> findMatchingBracket(LineStore const& lines, BufferPoint pos);
