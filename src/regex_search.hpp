#pragma once

/**
 * @file regex_search.hpp
 * @brief Per-line regex search/replace helpers used by Esc-X / Esc-Y.
 *
 * Kept separate from the smart-find helpers in search.hpp because regex
 * matching uses anchors, character classes and back-references that
 * smart-find deliberately avoids, and regex replace honours capture-group
 * substitution ($1, $&, …) so a plain replacement loop is the wrong
 * primitive. Matches do not span line boundaries — the same convention as
 * the rest of the editor's search/replace infrastructure.
 */

#include "buffer/buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief Compile @p pattern as an ECMAScript regex.
 * @param pattern The pattern source.
 * @param[out] error Set to a human-readable message on failure.
 * @return The compiled regex, or nullopt for an empty or invalid pattern
 *         (so the caller can surface @p error rather than let the
 *         exception escape).
 */
inline std::optional<std::regex> compileRegex(std::string const& pattern, std::string& error) {
  if (pattern.empty()) {
    error = "empty pattern";
    return std::nullopt;
  }
  try {
    return std::regex(pattern, std::regex::ECMAScript);
  } catch (std::regex_error const& e) {
    error = e.what();
    return std::nullopt;
  }
}

/**
 * @brief Match flags for a search that starts at byte @p from of a line.
 *
 * Mid-line, the text before @p from must still count as context so that
 * `^` does not match and `\b` / lookbehind-like tests see the previous
 * character.
 */
inline std::regex_constants::match_flag_type regexFlagsAt(std::size_t from) {
  return from > 0 ? std::regex_constants::match_prev_avail : std::regex_constants::match_default;
}

/**
 * @brief Scan a line for the first regex match starting at or after
 *        @p from.
 *
 * The text before @p from is context (see regexFlagsAt()), not a line start.
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>>
findFirstRegex(std::string const& line, std::size_t from, std::regex const& re) {
  if (from > line.size())
    return std::nullopt;
  std::cmatch m;
  // Use regex_search on the suffix; iterating with regex_iterator from
  // `from` would re-allocate the iterator's match buffer, regex_search
  // is the cheaper path for "first match only".
  auto begin = line.data() + from;
  auto end = line.data() + line.size();
  if (!std::regex_search(begin, end, m, re, regexFlagsAt(from)))
    return std::nullopt;
  std::size_t s = static_cast<std::size_t>(m[0].first - line.data());
  std::size_t e = static_cast<std::size_t>(m[0].second - line.data());
  return std::pair{s, e};
}

/**
 * @brief Scan a line for the last regex match within [from, limit).
 *
 * Used by the backward-search path.
 * @return {matchStart, matchEnd} byte offsets, or nullopt.
 */
inline std::optional<std::pair<std::size_t, std::size_t>>
findLastRegex(std::string const& line, std::size_t from, std::size_t limit, std::regex const& re) {
  std::optional<std::pair<std::size_t, std::size_t>> last;
  std::size_t pos = from;
  while (pos <= limit) {
    auto m = findFirstRegex(line, pos, re);
    if (!m || m->first >= limit)
      break;
    last = m;
    // Advance past zero-width matches to avoid an infinite loop.
    pos = (m->second > m->first) ? m->second : m->second + 1;
  }
  return last;
}

/**
 * @brief Find the next regex match in the buffer at or after @p start.
 *
 * Scans line by line and never wraps.
 * @return The matched region, or nullopt.
 */
inline std::optional<std::pair<BufferPoint, BufferPoint>>
findNextRegexInBuffer(Buffer const& buf, BufferPoint start, std::regex const& re) {
  std::size_t lineCount = buf.lineCount();
  for (std::size_t li = start.line; li < lineCount; ++li) {
    std::size_t from = (li == start.line) ? start.offset : 0;
    if (auto m = findFirstRegex(buf.lineContent(li), from, re))
      return std::pair{BufferPoint{li, m->first}, BufferPoint{li, m->second}};
  }
  return std::nullopt;
}

/**
 * @brief Substitute a single match's region with @p replacement,
 *        honouring capture-group back-references ($0..$9, $&, $$).
 *
 * The caller has already located the match's [from, to) and verified the
 * line.
 */
inline std::string substituteCaptures(std::string const& line, std::size_t from, std::size_t to,
                                      std::regex const& re, std::string const& replacement) {
  // Re-run the search anchored at `from` with the whole line as context,
  // so matches that depend on surrounding text (lookahead, \b, ^, $)
  // reproduce and $1.. resolve correctly.
  std::cmatch m;
  auto flags = regexFlagsAt(from) | std::regex_constants::match_continuous;
  if (!std::regex_search(line.data() + from, line.data() + line.size(), m, re, flags) ||
      m[0].second != line.data() + to)
    return replacement; // shouldn't happen, but fail safe
  return m.format(replacement, std::regex_constants::format_default);
}

/**
 * @brief Replace every forward regex match of @p re with the result of
 *        applying @p replacement (a sed-style template with $&/$N
 *        back-references) to each match.
 *
 * The scan resumes after the inserted text so a replacement that itself
 * matches the pattern does not loop. A no-op on a read-only buffer.
 *
 * @return The number of replacements performed.
 */
inline std::size_t replaceAllRegexForward(Buffer& buf, BufferPoint start, std::regex const& re,
                                          std::string const& replacement) {
  if (buf.readonly())
    return 0;
  std::size_t count = 0;
  BufferPoint cursor = start;
  while (auto m = findNextRegexInBuffer(buf, cursor, re)) {
    auto& line = buf.lineContent(m->first.line);
    auto rendered = substituteCaptures(line, m->first.offset, m->second.offset, re, replacement);
    buf.deleteRegion(m->first, m->second);
    cursor = buf.insertText(rendered, m->first);
    ++count;
  }
  return count;
}
