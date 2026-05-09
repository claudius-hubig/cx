#pragma once

#include "buffer/buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

// Per-line regex search helpers used by Esc-X / Esc-Y. Kept separate
// from the smart-find helpers in `search.hpp` because:
// - Regex matching uses anchors, character classes and back-references
//   that smart-find deliberately avoids.
// - Regex replace honours capture-group substitution ($1, $&, …) so a
//   plain replacement loop is the wrong primitive.
//
// Matches do not span line boundaries — same convention as the rest of
// the editor's search/replace infrastructure.

// Compile a pattern. Returns nullopt for invalid syntax so the caller
// can surface the std::regex error message in the status line rather
// than letting the exception escape.
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

// Scan a line for the first regex match starting at or after `from`.
// Returns {matchStart, matchEnd} byte offsets, or nullopt.
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
  if (!std::regex_search(begin, end, m, re))
    return std::nullopt;
  std::size_t s = static_cast<std::size_t>(m[0].first - line.data());
  std::size_t e = static_cast<std::size_t>(m[0].second - line.data());
  return std::pair{s, e};
}

// Scan a line for the last regex match within [from, limit). Used by
// the backward-search path.
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

// Find the next regex match in the buffer starting at `start`. Scans
// line-by-line; never wraps. Returns the matched region or nullopt.
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

// Substitute a single match's region with `replacement`, honouring
// capture-group back-references ($0..$9, $&, $$). Caller has already
// located the match's [from, to) and verified the line.
inline std::string substituteCaptures(std::string const& line, std::size_t from, std::size_t to,
                                      std::regex const& re, std::string const& replacement) {
  // Run regex_match on the exact region so $1.. resolve correctly.
  auto matchTextStart = line.data() + from;
  auto matchTextEnd = line.data() + to;
  std::cmatch m;
  if (!std::regex_match(matchTextStart, matchTextEnd, m, re))
    return replacement; // shouldn't happen, but fail safe
  return m.format(replacement, std::regex_constants::format_default);
}

// Replace every forward regex match of `re` with the result of running
// `replacement` (a sed-style template with $&/$N back-references)
// against each match. Scan resumes after the inserted text so a
// replacement that itself matches the pattern does not loop. Returns
// the number of replacements performed.
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
