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

  bool operator==(BufferPoint const&) const = default;

  auto operator<(BufferPoint const& o) const {
    if (line != o.line)
      return line < o.line;
    return offset < o.offset;
  }
  auto operator<=(BufferPoint const& o) const { return *this == o || *this < o; }
  auto operator>(BufferPoint const& o) const { return o < *this; }
  auto operator>=(BufferPoint const& o) const { return o <= *this; }
};

// Viewport position: the first visible physical line and an additional offset
// within that line.
//
// In wrap mode `col` is a byte offset into the first visible physical line,
// allowing the viewport to start partway through a long wrapped line.
// In no-wrap mode `col` is a visual-column horizontal-scroll offset applied
// uniformly to all rendered lines.
struct ViewFrom {
  RowIdx line{0};
  ColIdx col{0};

  bool operator==(ViewFrom const&) const = default;
};
