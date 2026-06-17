#pragma once

/**
 * @file replace.hpp
 * @brief Exact-match find-and-replace helpers used by
 *        Screen::openReplacePrompt.
 *
 * Replace deliberately uses byte-exact matching (not the smart-find rules
 * in search.hpp): fuzzy skipping is helpful for locating text you can
 * already see, but dangerous when the editor is about to rewrite bytes
 * you may not have intended to touch.
 */

#include "buffer/buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

/**
 * @brief Find the next exact substring match of @p pattern at or after
 *        @p start in @p buf.
 *
 * Scans line by line and never wraps past end-of-buffer; matches do not
 * span line boundaries.
 *
 * @return The matched region as {from, to}, or nullopt when no further
 *         match exists.
 */
inline std::optional<std::pair<BufferPoint, BufferPoint>>
findNextMatchInBuffer(Buffer const& buf, BufferPoint start, std::string const& pattern) {
  if (pattern.empty())
    return std::nullopt;
  std::size_t lineCount = buf.lineCount();
  for (std::size_t li = start.line; li < lineCount; ++li) {
    std::size_t from = (li == start.line) ? start.offset : 0;
    auto const& line = buf.lineContent(li);
    auto pos = line.find(pattern, from);
    if (pos != std::string::npos)
      return std::pair{BufferPoint{li, pos}, BufferPoint{li, pos + pattern.size()}};
  }
  return std::nullopt;
}

/**
 * @brief Replace every forward exact-match occurrence of @p pattern with
 *        @p replacement from @p start to end-of-buffer.
 *
 * After each replacement the scan resumes immediately after the inserted
 * text, guaranteeing termination even when @p replacement itself matches
 * @p pattern (the inserted text is not re-scanned). A no-op on a
 * read-only buffer or an empty pattern.
 *
 * @return The number of replacements performed.
 */
inline std::size_t replaceAllForward(Buffer& buf, BufferPoint start, std::string const& pattern,
                                     std::string const& replacement) {
  if (pattern.empty() || buf.readonly())
    return 0;
  std::size_t count = 0;
  BufferPoint cursor = start;
  while (auto m = findNextMatchInBuffer(buf, cursor, pattern)) {
    buf.deleteRegion(m->first, m->second);
    cursor = buf.insertText(replacement, m->first);
    ++count;
  }
  return count;
}
