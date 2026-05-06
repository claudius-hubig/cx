#pragma once

#include "buffer/buffer.hpp"
#include "search.hpp"
#include "types.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

// Find the next forward smart-find match starting at `start` in `buf`,
// scanning line-by-line and never wrapping past end-of-buffer. Matches do
// not span line boundaries (consistent with Find). Returns the matched
// region as {from, to} or nullopt when no further match exists.
inline std::optional<std::pair<BufferPoint, BufferPoint>>
findNextMatchInBuffer(Buffer const& buf, BufferPoint start, std::string const& pattern,
                      FindFlags const& f) {
  std::size_t lineCount = buf.lineCount();
  for (std::size_t li = start.line; li < lineCount; ++li) {
    std::size_t from = (li == start.line) ? start.offset : 0;
    if (auto m = findFirstMatch(buf.lineContent(li), from, pattern, f))
      return std::pair{BufferPoint{li, m->first}, BufferPoint{li, m->second}};
  }
  return std::nullopt;
}

// Replace every forward smart-find occurrence of `pattern` with `replacement`
// from `start` to end-of-buffer. After each replacement the scan resumes at
// the position immediately after the inserted text — guaranteeing termination
// even when `replacement` itself matches `pattern` (the inserted text is not
// re-scanned). Returns the number of replacements performed.
inline std::size_t replaceAllForward(Buffer& buf, BufferPoint start, std::string const& pattern,
                                     std::string const& replacement) {
  if (pattern.empty() || buf.readonly())
    return 0;
  FindFlags f(pattern);
  std::size_t count = 0;
  BufferPoint cursor = start;
  while (auto m = findNextMatchInBuffer(buf, cursor, pattern, f)) {
    buf.deleteRegion(m->first, m->second);
    cursor = buf.insertText(replacement, m->first);
    ++count;
  }
  return count;
}
