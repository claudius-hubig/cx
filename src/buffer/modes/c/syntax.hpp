#pragma once

/**
 * @file c/syntax.hpp
 * @brief The C/C++ tokenizer plus the shared syntax vocabulary used by
 *        every mode: the SyntaxToken enum, the cross-line SyntaxState, and
 *        bracket matching.
 *
 * Each mode's tokenizer writes into the same SyntaxToken set and threads
 * the same SyntaxState across lines, so the buffer's per-line syntax cache
 * is mode-agnostic.
 */

#include "types.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
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
 * @brief State of a cross-line Markdown link.
 *
 * Tracks a `[text](url)` link that spans lines: @c Label while the `[...`
 * label is still open (no `]` yet), @c Url while the `](...` destination is
 * still open (no `)` yet). An unterminated link stays Link-coloured to end
 * of file, matching the optimistic behaviour of the raw-string / heredoc /
 * triple-string states.
 */
enum class MdLink : uint8_t { None, Label, Url };

/**
 * @brief Multi-line tokenizer state carried between lines.
 *
 * One instance is cached per line. Invariant: at the start of any line at
 * most one of {inBlockComment, rawDelim.has_value(), inPreprocessor} is
 * true. Because a buffer has exactly one mode, the C/C++ fields and the
 * per-mode fields are never active simultaneously: a shell buffer only
 * sets @c heredocTag, a Python buffer only @c pyTriple, a YAML buffer only
 * @c yamlBlockCol, a Markdown buffer only @c mdLink.
 */
struct SyntaxState {
  bool inBlockComment = false;
  bool inPreprocessor = false; /**< Next line continues a \#directive (prior ended with '\'). */
  std::optional<std::string> rawDelim; /**< has_value() = inside R"delim(...)delim". */

  /**
   * @brief Shell heredoc tag: has_value() means subsequent lines are
   *        heredoc body (String) until a line equal to the tag.
   */
  std::optional<std::string> heredocTag;
  /** @brief Set for the `<<-` form, where leading tabs are stripped before the
   *         terminator comparison. */
  bool heredocStripTabs = false;

  /**
   * @brief Python triple-quote: 0 = not inside; '\'' or '"' = the open
   *        '''/""" quote character.
   */
  char pyTriple = 0;
  /** @brief Suppresses backslash-escape handling for r"""..."""-style strings. */
  bool pyTripleRaw = false;

  /**
   * @brief YAML block scalar: >= 0 = inside a `|`/`>` scalar introduced at
   *        the given indent column (deeper/blank lines are String content
   *        until indentation returns to <= this column); -1 = not in one.
   */
  int yamlBlockCol = -1;

  /** @brief Markdown cross-line link state (see MdLink). */
  MdLink mdLink = MdLink::None;
};

/**
 * @brief Tokenize a single line, writing per-byte SyntaxToken values into
 *        @p out.
 *
 * @p out is resized/overwritten; reusing a vector lets the caller reuse
 * heap allocations. @p state is the multi-line state entering this line and
 * is updated on return to reflect the state after it.
 */
void tokenizeLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& out);

/**
 * @brief Advance multi-line state across a line without producing tokens.
 *
 * Cheaper than tokenizeLine(): skips identifier/keyword classification,
 * operator/bracket tagging, and the per-byte token vector allocation. Only
 * tracks block-comment and raw-string-literal state.
 */
void advanceBlockCommentState(std::string const& line, SyntaxState& state);

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
std::optional<BufferPoint> findMatchingBracket(std::vector<LinePtr> const& lines, BufferPoint pos,
                                               LineTokensFn const& getTokens);
/**
 * @brief Convenience overload that tokenizes lines lazily and memoizes them
 *        for the call's lifetime.
 *
 * IMPORTANT: this overload always tokenizes from line 0 to the target line
 * on first access (to maintain the cross-line SyntaxState chain), so its
 * cost is O(file size) — not just the scan distance. Intended for tests and
 * ad-hoc use; production hot paths should thread cachedLineTokens() through
 * the callback overload instead.
 */
std::optional<BufferPoint> findMatchingBracket(std::vector<LinePtr> const& lines, BufferPoint pos);
