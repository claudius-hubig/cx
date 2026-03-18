#pragma once

#include <cstddef>
#include <variant>

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

// Sub-line viewport offset for wrap mode: byte offset within the viewport's
// first line. Used to start rendering mid-way through a very long wrapped line.
struct WrapOffset {
  std::size_t bytes = 0;
};

// Horizontal scroll for no-wrap mode: visual-column offset applied to all
// lines. Must be visual columns (not bytes) because the same visual position
// maps to different byte offsets in different lines.
struct HScrollOffset {
  std::size_t cols = 0;
};

// The viewport's horizontal offset. Only one interpretation is active at a
// time, determined by the wrap parameter passed to render/ensureVisible.
using ViewOffset = std::variant<WrapOffset, HScrollOffset>;
