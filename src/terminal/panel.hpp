#pragma once

/**
 * @file panel.hpp
 * @brief Panel — one pane of the terminal: a rectangle, the BufferView it
 *        shows, and the overlay-restore slot.
 */

#include "buffer/buffer.hpp"
#include "buffer/buffer_view.hpp"
#include "types.hpp"

#include <cstddef>
#include <memory>

/** @brief Rectangle in absolute terminal coordinates. */
struct PanelRect {
  std::size_t row = 0;  /**< Top row (0-based). */
  std::size_t col = 0;  /**< Left column (0-based). */
  std::size_t rows = 0; /**< Height. */
  std::size_t cols = 0; /**< Width (including gutter). */
};

/**
 * @brief One pane of the terminal, showing a single buffer view.
 *
 * Each panel has its own cursor, viewport, and modeline.
 */
class Panel {
public:
  /**
   * @brief Construct a panel with @p id, buffer @p buf, and rectangle
   *        @p rect.
   *
   * The initial BufferView copies state from the buffer's saved fields.
   */
  Panel(int id, Buffer& buf, PanelRect rect);

  /** @brief This panel's stable id (1-based). */
  int id() const { return _id; }
  BufferView& view() { return *_view; }
  BufferView const& view() const { return *_view; }
  Buffer& buffer() { return _view->buffer(); }
  Buffer const& buffer() const { return _view->buffer(); }
  PanelRect const& rect() const { return _rect; }
  void setRect(PanelRect r) { _rect = r; }

  /** @brief Rows available for buffer content (rect.rows minus the modeline). */
  RowIdx contentRows() const;

  /** @brief Total columns including gutter. */
  ColIdx totalCols() const;

  /**
   * @brief Switch this panel to a different buffer.
   * @param buf The buffer to show.
   * @param restoreState When true, the new BufferView copies state from the
   *        buffer's saved fields; otherwise it starts at position (0,0).
   */
  void setBuffer(Buffer& buf, bool restoreState);

  /**
   * @brief Save the current view's state back into the buffer's saved
   *        fields.
   *
   * Called before the view is dropped (buffer switch or panel close) so
   * the cursor position survives.
   */
  void saveViewState();

  /**
   * @brief Overlay restore slot.
   *
   * When non-null, this panel is showing a read-only overlay
   * (help/buffer-list/paste-ring) and @c prevBuf points to the buffer
   * underneath, to be restored when the overlay is dismissed.
   */
  Buffer* prevBuf = nullptr;

private:
  int _id;
  std::unique_ptr<BufferView> _view;
  PanelRect _rect;
};
