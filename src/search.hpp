#pragma once

/**
 * @file search.hpp
 * @brief Smart-find matching helpers used by Screen::findNext / findPrev.
 *
 * Header-only and pure so tests can exercise them directly without
 * pulling in the Screen/Buffer machinery.
 *
 * Smart-find rules:
 * - Case-insensitive unless the pattern contains an uppercase letter.
 * - Digits in the buffer are skipped between matched characters unless
 *   the pattern contains digits.
 * - Special (non-letter, non-digit) characters in the buffer are skipped
 *   between matched characters unless the pattern contains any.
 * - The first character must match at the exact offset; subsequent
 *   characters may have skipped chars between them.
 * - Case folding is ASCII-only.
 */

#include "utf8.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief Flags derived from a search pattern that govern which characters
 *        are skipped in the target line.
 */
struct FindFlags {
  bool caseSensitive = false; /**< Set if the pattern contains an uppercase letter. */
  bool matchDigits = false;   /**< Set if the pattern contains a digit. */
  bool matchSpecial = false;  /**< Set if the pattern contains a non-alphanumeric char. */
  char firstChar = 0;         /**< Exact first char of the pattern. */
  char firstLower = 0;        /**< Lowercased first char. */
  char firstUpper = 0;        /**< Uppercased first char. */

  /**
   * @brief Derive the flags from @p pattern.
   * @param pattern The search pattern.
   */
  explicit FindFlags(std::string_view pattern) {
    for (auto c : pattern) {
      auto u = static_cast<unsigned char>(c);
      if (u >= 'A' && u <= 'Z')
        caseSensitive = true;
      if (u >= '0' && u <= '9')
        matchDigits = true;
      bool isLetter = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
      bool isDigit = (u >= '0' && u <= '9');
      if (!isLetter && !isDigit)
        matchSpecial = true;
    }
    firstChar = pattern.empty() ? 0 : pattern[0];
    auto lower = [](char c) -> char {
      return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    };
    firstLower = lower(firstChar);
    firstUpper =
        (firstLower >= 'a' && firstLower <= 'z') ? static_cast<char>(firstLower - 32) : firstLower;
  }

  /** @brief True if @p c could begin a match under the case rules. */
  bool firstCharCanMatch(char c) const {
    if (caseSensitive)
      return c == firstChar;
    return c == firstLower || c == firstUpper;
  }
};

/**
 * @brief Test whether @p pattern matches @p line starting at byte offset
 *        @p off.
 *
 * Ignored characters in the buffer line are skipped between matched
 * chars, but the first pattern character must match directly at @p off.
 *
 * @return The byte offset past the last matched character, or nullopt on
 *         no match.
 */
inline std::optional<std::size_t> matchAt(std::string const& line, std::size_t off,
                                          std::string const& pattern, bool caseSensitive,
                                          bool matchDigits, bool matchSpecial) {
  if (pattern.empty())
    return off;
  auto lower = [](char c) -> char {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
  };

  std::size_t pi = 0, li = off;

  // First character must match directly (no skipping).
  if (li >= line.size())
    return std::nullopt;
  if (caseSensitive ? (pattern[pi] != line[li]) : (lower(pattern[pi]) != lower(line[li])))
    return std::nullopt;
  ++pi;
  ++li;

  // Remaining characters: skip ignored chars between matches.
  while (pi < pattern.size()) {
    while (li < line.size()) {
      auto u = static_cast<unsigned char>(line[li]);
      if (u >= 0x80) {
        if (!matchSpecial) {
          li += utf8SeqLen(u);
          continue;
        }
        break;
      }
      bool isLetter = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
      bool isDigit = (u >= '0' && u <= '9');
      if (!matchDigits && isDigit) {
        ++li;
        continue;
      }
      if (!matchSpecial && !isLetter && !isDigit) {
        ++li;
        continue;
      }
      break;
    }
    if (li >= line.size())
      return std::nullopt;
    if (caseSensitive ? (pattern[pi] != line[li]) : (lower(pattern[pi]) != lower(line[li])))
      return std::nullopt;
    ++pi;
    ++li;
  }
  return li;
}

/**
 * @brief Scan a line for the first match starting at or after @p from.
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>> findFirstMatch(std::string const& line,
                                                                         std::size_t from,
                                                                         std::string const& pattern,
                                                                         FindFlags const& f) {
  for (std::size_t off = from; off < line.size(); ++off) {
    if (!f.firstCharCanMatch(line[off]))
      continue;
    if (auto end = matchAt(line, off, pattern, f.caseSensitive, f.matchDigits, f.matchSpecial))
      return std::pair{off, *end};
  }
  return std::nullopt;
}

/**
 * @brief Scan a line for the last match within [from, limit).
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>>
findLastMatch(std::string const& line, std::size_t from, std::size_t limit,
              std::string const& pattern, FindFlags const& f) {
  std::optional<std::pair<std::size_t, std::size_t>> last;
  for (std::size_t off = from; off < limit && off < line.size(); ++off) {
    if (!f.firstCharCanMatch(line[off]))
      continue;
    if (auto end = matchAt(line, off, pattern, f.caseSensitive, f.matchDigits, f.matchSpecial))
      last = {off, *end};
  }
  return last;
}
