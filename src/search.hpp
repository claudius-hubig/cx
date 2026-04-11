#pragma once

#include "utf8.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Pure-function search helpers used by Screen::findNext/findPrev. Kept
// header-only so tests can exercise them directly without pulling in
// the Screen/Buffer machinery.
//
// Smart-find rules:
// - Case-insensitive unless the pattern contains an uppercase letter.
// - Digits in the buffer are skipped between matched characters unless
//   the pattern contains digits.
// - Special (non-letter, non-digit) characters in the buffer are
//   skipped between matched characters unless the pattern contains any.
// - The first character must match at the exact offset; subsequent
//   characters may have skipped chars between them.
// - Case folding is ASCII-only.

// Flags derived from a search pattern that govern what characters get
// skipped in the target line.
struct FindFlags {
  bool caseSensitive = false;
  bool matchDigits = false;
  bool matchSpecial = false;
  char firstChar = 0;  // exact first char of pattern
  char firstLower = 0; // lowercased
  char firstUpper = 0; // uppercased

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

  bool firstCharCanMatch(char c) const {
    if (caseSensitive)
      return c == firstChar;
    return c == firstLower || c == firstUpper;
  }
};

// Check if `pattern` matches `line` starting at byte offset `off`.
// Ignored characters in the buffer line are skipped between matched
// chars, but the first pattern character must match directly at `off`.
// Returns the byte offset past the last matched character, or nullopt
// on no match.
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

// Scan a line for the first match starting at or after `from`.
// Returns {matchStart, matchEnd} byte offsets, or nullopt.
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

// Scan a line for the last match within [from, limit). Returns
// {matchStart, matchEnd} byte offsets, or nullopt.
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
