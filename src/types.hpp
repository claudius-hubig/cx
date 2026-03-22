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

// Content width: terminal columns minus the gutter column, minimum 1.
inline std::size_t contentWidth(ColIdx cols) { return cols.value > 1 ? cols.value - 1 : 1; }

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

// Byte offset into a line (used as sub-line scroll position in wrap mode).
struct ByteOffset {
  std::size_t value = 0;
  bool operator==(ByteOffset const&) const = default;
};

// Visual column index (used as horizontal scroll in no-wrap mode).
struct VisualCol {
  std::size_t value = 0;
  bool operator==(VisualCol const&) const = default;
};

// Viewport position: the first visible physical line and a mode-dependent
// sub-position within it.
//
// In wrap mode the variant holds ByteOffset — a byte offset into the first
// visible physical line, allowing the viewport to start partway through a
// long wrapped line.
//
// In no-wrap mode the variant holds VisualCol — a visual-column
// horizontal-scroll offset applied uniformly to all rendered lines.
struct ViewFrom {
  RowIdx line{0};
  std::variant<ByteOffset, VisualCol> col{ByteOffset{0}};

  // Access the byte offset (wrap mode sub-line scroll). Caller must ensure
  // the variant currently holds ByteOffset.
  std::size_t byteOffset() const { return std::get<ByteOffset>(col).value; }
  std::size_t& byteOffset() { return std::get<ByteOffset>(col).value; }

  // Access the visual column (no-wrap mode horizontal scroll). Caller must
  // ensure the variant currently holds VisualCol.
  std::size_t hScroll() const { return std::get<VisualCol>(col).value; }
  std::size_t& hScroll() { return std::get<VisualCol>(col).value; }

  // Reset the sub-position to zero, selecting the correct variant type.
  void resetCol(bool wrap) {
    col = wrap ? decltype(col){ByteOffset{0}} : decltype(col){VisualCol{0}};
  }

  bool operator==(ViewFrom const&) const = default;
};
