#pragma once

#include "buffer/buffer.hpp"
#include "buffer/buffer_view.hpp"
#include "types.hpp"

#include <cstddef>
#include <memory>

/// Rectangle in absolute terminal coordinates.
struct PanelRect {
  std::size_t row = 0;  // top row (0-based)
  std::size_t col = 0;  // left column (0-based)
  std::size_t rows = 0; // height
  std::size_t cols = 0; // width (including gutter)
};

/// A panel is one pane of the terminal, showing a single buffer view.
/// Each panel has its own cursor, viewport, and modeline.
class Panel {
public:
  /// Construct a panel with the given id, buffer, and rectangle.
  /// The initial BufferView copies state from the buffer's fields.
  Panel(int id, Buffer& buf, PanelRect rect);

  int id() const { return _id; }
  BufferView& view() { return *_view; }
  BufferView const& view() const { return *_view; }
  Buffer& buffer() { return _view->buffer(); }
  Buffer const& buffer() const { return _view->buffer(); }
  PanelRect const& rect() const { return _rect; }
  void setRect(PanelRect r) { _rect = r; }

  /// Rows available for buffer content (rect.rows minus 1 for modeline).
  RowIdx contentRows() const;

  /// Total columns including gutter.
  ColIdx totalCols() const;

  /// Switch this panel to a different buffer. If restoreState is true,
  /// the new BufferView copies state from the buffer's saved fields;
  /// otherwise it starts at position (0,0).
  void setBuffer(Buffer& buf, bool restoreState);

  /// Save the current view's state back into the buffer's fields.
  /// Call this before destroying the view when it's the last view
  /// for this buffer, to preserve cursor position.
  void saveViewState();

  /// Per-panel overlay buffer (for help/bufferlist/pastering).
  /// When non-null, this panel is showing a readonly overlay and
  /// prevBuf points to the buffer underneath.
  Buffer* prevBuf = nullptr;

private:
  int _id;
  std::unique_ptr<BufferView> _view;
  PanelRect _rect;
};
