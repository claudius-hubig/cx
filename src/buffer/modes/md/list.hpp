#pragma once

/**
 * @file md/list.hpp
 * @brief List item markers, as Markdown writes them.
 */

#include <cstddef>
#include <string_view>

/**
 * @brief Length of the list item marker at byte @p off of @p line,
 *        including the blanks after it; 0 if there is none.
 *
 * A marker is `-`, `*` or `+`, or digits followed by `.` or `)`, and a
 * space or tab must follow it. The one place that knows this, shared by
 * Alt-q (in every mode: each item is reflowed on its own) and the Markdown
 * tokenizer (an item ends a link left open above it), so the two agree on
 * where an item starts. @p off is normally the line's first non-blank byte.
 */
constexpr std::size_t listMarkerLength(std::string_view line, std::size_t off) {
  auto blankAt = [&](std::size_t k) {
    return k < line.size() && (line[k] == ' ' || line[k] == '\t');
  };
  std::size_t k = off;
  if (k < line.size() && (line[k] == '-' || line[k] == '*' || line[k] == '+')) {
    ++k;
  } else {
    while (k < line.size() && line[k] >= '0' && line[k] <= '9')
      ++k;
    if (k == off || k == line.size() || (line[k] != '.' && line[k] != ')'))
      return 0;
    ++k;
  }
  if (!blankAt(k))
    return 0;
  while (blankAt(k))
    ++k;
  return k - off;
}
