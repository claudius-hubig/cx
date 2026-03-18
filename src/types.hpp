#pragma once

#include <cstddef>

struct RowIdx {
  explicit RowIdx(std::size_t value) : value(value) {}
  std::size_t value;
};

struct ColIdx {
  explicit ColIdx(std::size_t value) : value(value) {}
  std::size_t value;
};

// Position within a Buffer: line index (0-based) + byte offset within that
// line. An offset equal to the line's size represents the "newline position"
// (logically past the last character of the line).
struct BufferPoint {
  std::size_t line = 0;
  std::size_t offset = 0;

  bool operator==(BufferPoint const &) const = default;

  auto operator<(BufferPoint const &o) const {
    if (line != o.line)
      return line < o.line;
    return offset < o.offset;
  }
  auto operator<=(BufferPoint const &o) const {
    return *this == o || *this < o;
  }
  auto operator>(BufferPoint const &o) const { return o < *this; }
  auto operator>=(BufferPoint const &o) const { return o <= *this; }
};
