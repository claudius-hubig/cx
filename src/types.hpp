#pragma once

/**
 * @file types.hpp
 * @brief Small value types shared across the editor: line storage,
 *        buffer positions, and viewport descriptors.
 *
 * These types form the vocabulary used between the buffer layer and the
 * terminal layer. They carry no behaviour beyond trivial accessors so
 * they can live header-only and be freely copied.
 */

#include <compare>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>

/**
 * @brief Shared pointer to one line's text.
 *
 * Used by LineStore, Buffer and VersionHistory. The string is treated as
 * immutable while shared: the only mutation path is
 * LineStore::mutableLine() (via Buffer::mutLine()), which first detaches
 * a private copy if the line is shared and edits in place only an
 * unshared line created by makeLine() (line_store.hpp). Sharing lets
 * snapshots and the live buffer reference unchanged lines without
 * copying them.
 */
using LinePtr = std::shared_ptr<std::string const>;

/** @brief Strongly-typed row count / row index (0-based). */
struct RowIdx {
  explicit RowIdx(std::size_t value) : value(value) {}
  std::size_t value;
};

/** @brief Strongly-typed column count / column index (0-based). */
struct ColIdx {
  explicit ColIdx(std::size_t value) : value(value) {}
  std::size_t value;
};

/**
 * @brief Usable content width for a panel: terminal columns minus the
 *        one-column gutter, clamped to a minimum of 1.
 * @param cols Total columns available to the panel.
 */
inline std::size_t contentWidth(ColIdx cols) { return cols.value > 1 ? cols.value - 1 : 1; }

/**
 * @brief A position within a Buffer.
 *
 * @c line is a 0-based physical line index; @c offset is a byte offset
 * within that line. An @c offset equal to the line's size denotes the
 * "newline position" — logically just past the last character.
 */
struct BufferPoint {
  std::size_t line = 0;
  std::size_t offset = 0;

  auto operator<=>(BufferPoint const&) const = default;
};

/** @brief Byte offset into a line; the wrap-mode sub-line scroll position. */
struct ByteOffset {
  std::size_t value = 0;
  bool operator==(ByteOffset const&) const = default;
};

/** @brief Visual column index; the no-wrap-mode horizontal scroll offset. */
struct VisualCol {
  std::size_t value = 0;
  bool operator==(VisualCol const&) const = default;
};

/**
 * @brief Viewport anchor: the first visible physical line plus a
 *        mode-dependent sub-position within it.
 *
 * In wrap mode the variant holds a ByteOffset — a byte offset into the
 * first visible physical line, so the viewport may begin partway through
 * a long wrapped line. In no-wrap mode it holds a VisualCol — a uniform
 * horizontal-scroll offset applied to every rendered line.
 */
struct ViewFrom {
  RowIdx line{0};
  std::variant<ByteOffset, VisualCol> col{ByteOffset{0}};

  /**
   * @brief Read the wrap-mode byte offset.
   * @pre The variant currently holds a ByteOffset.
   */
  std::size_t byteOffset() const { return std::get<ByteOffset>(col).value; }

  /**
   * @brief Mutable access to the wrap-mode byte offset.
   * @pre The variant currently holds a ByteOffset.
   */
  std::size_t& byteOffset() { return std::get<ByteOffset>(col).value; }

  /**
   * @brief Read the no-wrap-mode horizontal scroll column.
   * @pre The variant currently holds a VisualCol.
   */
  std::size_t hScroll() const { return std::get<VisualCol>(col).value; }

  /**
   * @brief Mutable access to the no-wrap-mode horizontal scroll column.
   * @pre The variant currently holds a VisualCol.
   */
  std::size_t& hScroll() { return std::get<VisualCol>(col).value; }

  /**
   * @brief Reset the sub-position to zero, selecting the variant type
   *        appropriate for the current mode.
   * @param wrap True for wrap mode (ByteOffset), false for no-wrap (VisualCol).
   */
  void resetCol(bool wrap) {
    col = wrap ? decltype(col){ByteOffset{0}} : decltype(col){VisualCol{0}};
  }

  bool operator==(ViewFrom const&) const = default;
};

/**
 * @brief Suggested view placement returned by buffers that rebuild their
 *        own contents (e.g. DirBuffer::refresh, BufferListBuffer::refresh).
 *
 * The caller (BufferView / Screen) uses the hint to position the cursor
 * and viewport after the rebuild.
 */
struct ViewHint {
  BufferPoint point{};
  ViewFrom viewFrom{};
};
