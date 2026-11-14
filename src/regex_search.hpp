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
#include "utf8.hpp"

#include <algorithm>
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
 * @brief The position just past @p p for resuming a scan after an empty
 *        (zero-width) match at @p p.
 *
 * Steps one code point forward, or to the start of the next line from a
 * line end, so a scan that resumes there cannot find the same empty match
 * again.
 * @return The next position, or nullopt at the end of the buffer.
 */
inline std::optional<BufferPoint> stepPastEmptyMatch(Buffer const& buf, BufferPoint p) {
  auto const& line = buf.lineContent(p.line);
  if (p.offset < line.size()) {
    auto len = utf8SeqLen(static_cast<unsigned char>(line[p.offset]));
    return BufferPoint{p.line, std::min(p.offset + len, line.size())};
  }
  if (p.line + 1 < buf.lineCount())
    return BufferPoint{p.line + 1, 0};
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
 * Every line is matched once, left to right, against its original text
 * and rewritten in one edit. Matches therefore never see replaced text:
 * a replacement that itself matches does not loop, `^a` replaces only
 * the `a` that started the line (not the one that moves there once it
 * is removed), and a line costs time linear in its length however many
 * matches it has. An empty match is replaced and the scan steps one code
 * point on, so zero-width patterns (`x*`, `^`, `\b`, …) terminate. (Unlike
 * std::regex_replace, no non-empty match is retried at the position of an
 * empty one, which only matters for patterns such as `|a`.) A no-op on a
 * read-only buffer.
 *
 * The edits bypass BufferView, so this records the history version itself:
 * one version, with the cursor at @p start, when anything was replaced.
 *
 * @return The number of replacements performed.
 */
inline std::size_t replaceAllRegexForward(Buffer& buf, BufferPoint start, std::regex const& re,
                                          std::string const& replacement) {
  if (buf.readonly())
    return 0;
  std::size_t count = 0;
  for (std::size_t li = start.line; li < buf.lineCount(); ++li) {
    std::string const line = buf.lineContent(li); // a copy: the buffer's line is replaced below
    std::size_t pos = li == start.line ? std::min(start.offset, line.size()) : 0;
    std::string out(line, 0, pos);
    std::size_t before = count;
    while (auto m = findFirstRegex(line, pos, re)) {
      bool empty = m->first == m->second;
      out.append(line, pos, m->first - pos);
      out += substituteCaptures(line, m->first, m->second, re, replacement);
      ++count;
      pos = m->second;
      if (empty) {
        if (pos >= line.size())
          break;
        auto len = std::min(utf8SeqLen(static_cast<unsigned char>(line[pos])), line.size() - pos);
        out.append(line, pos, len);
        pos += len;
      }
    }
    if (count == before)
      continue;
    out.append(line, pos);
    buf.deleteRegion({li, 0}, {li, line.size()});
    li = buf.insertText(out, {li, 0}).line; // a replacement may contain newlines
  }
  if (count > 0)
    buf.recordVersion(start);
  return count;
}
