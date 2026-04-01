#pragma once

#include "buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

/// Per-view state for a Buffer. Owns the cursor position, selection mark,
/// viewport scroll, and per-view caches. Delegates all operations to the
/// underlying Buffer via a sync pattern: view state is copied into Buffer's
/// fields before the call, and copied back afterwards.
///
/// Multiple BufferViews can reference the same Buffer (for multi-panel
/// editing). Each view maintains independent cursor/viewport state.
class BufferView {
public:
  /// Construct a view with default position (0,0) for the given buffer.
  explicit BufferView(Buffer& buf);

  /// Construct a view by copying the buffer's current view state. Used when
  /// the buffer was previously shown in a single view and we want to resume
  /// the saved cursor position.
  static BufferView fromBufferState(Buffer& buf);

  Buffer& buffer() { return _buf; }
  Buffer const& buffer() const { return _buf; }

  // --- View state accessors ---
  BufferPoint point() const { return _point; }
  std::optional<BufferPoint> mark() const { return _mark; }
  BufferPoint viewFrom() const;
  ViewFrom const& viewFromRaw() const { return _viewFrom; }

  void setPoint(std::size_t line, std::size_t offset);
  void setMark(BufferPoint p) { _mark = p; }
  void clearMark() { _mark.reset(); }
  void toggleMark();

  void setHighlight(BufferPoint from, BufferPoint to) { _highlight = Buffer::Highlight{from, to}; }
  void clearHighlight() { _highlight.reset(); }

  std::tuple<std::size_t, std::size_t> lineCol() const;

  // --- Movement ---
  void moveLeft();
  void moveRight();
  void moveUp(ColIdx cols);
  void moveDown(ColIdx cols);
  void movePhysicalUp();
  void movePhysicalDown();
  void moveHome();
  void moveEnd();
  void moveWordRight();
  void moveWordLeft();
  void moveParagraphUp();
  void moveParagraphDown();
  void pageUp(RowIdx screenRows, ColIdx cols, bool wrap);
  void pageDown(RowIdx screenRows, ColIdx cols, bool wrap);
  bool gotoMatchingBracket();
  void gotoLine(std::size_t line, RowIdx screenRows);
  void scrollDown();
  void scrollUp(bool wrap = false);
  void scrollViewNearEnd(RowIdx screenRows);

  // --- Editing ---
  void insert(uint32_t cp);
  void deleteAtPoint();
  void deleteBeforePoint();
  void deleteChunkAtPoint();
  void deleteChunkBeforePoint();
  void adjustCursorToIndent();

  // Region operations.
  std::string extractRegion(BufferPoint from, BufferPoint to) const;
  void deleteRegion(BufferPoint from, BufferPoint to);
  BufferPoint insertText(std::string_view text);

  // --- Rendering ---
  void ensureVisible(RowIdx rows, ColIdx cols, bool wrap);
  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    bool showWhitespace = false, bool wrap = true,
                                    std::size_t rowOffset = 0, std::size_t colOffset = 0) const;
  std::size_t screenRow(ColIdx cols, bool wrap = true) const;
  bool currentLineWraps(ColIdx cols) const;

  /// Clamp point/mark to valid buffer positions after external edits.
  /// Call this when another view may have modified the buffer's content.
  void clampToBuffer();

  /// Save this view's state back into the buffer's fields. Used when this
  /// is the last view before destruction, to preserve cursor position.
  void saveToBuffer();

private:
  /// Copy view state into Buffer's protected fields before delegating.
  void syncTo() const;
  /// Copy view state back from Buffer's fields after delegating.
  void syncFrom();

  Buffer& _buf;
  BufferPoint _point;
  std::optional<BufferPoint> _mark;
  ViewFrom _viewFrom;
  bool _wrapMode = false;
  std::optional<ColIdx> _desiredCol;
  std::optional<Buffer::Highlight> _highlight;

  mutable Buffer::PointColCache _pointColCache;
  mutable Buffer::RowBoundsCache _rowBoundsCache;

  std::size_t _docVersion = 0;
};
