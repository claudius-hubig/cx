#pragma once

#include "buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/// Display settings passed to BufferView::render().
struct BufferRenderArgs {
  bool showWhitespace = false;
  bool wrap = true;
  bool focused = true;
  std::size_t rowOffset = 0;
  std::size_t colOffset = 0;
};

/// Per-view state for a Buffer. Owns the cursor position, selection mark,
/// viewport scroll, wrap mode, desired column, find highlight, and
/// per-view caches. Movement, editing, and rendering are implemented
/// directly using the Buffer's public accessors (lineContent, lineCount,
/// isBlankLine, lines) and its position-explicit editing API
/// (insertAt, deleteAt, deleteBefore, insertText, deleteRegion).
///
/// Multiple BufferViews can reference the same Buffer (for multi-panel
/// editing). Each view maintains independent cursor/viewport state and
/// detects external edits via Buffer::docVersion(), clamping its
/// positions on the next access.
class BufferView {
public:
  /// Construct a view with default position (0,0) for the given buffer.
  explicit BufferView(Buffer& buf);

  /// Construct a view from the buffer's saved view state (if any).
  /// If the buffer has no saved state, starts at position (0,0).
  /// Clears the saved state after reading it.
  static BufferView fromSavedState(Buffer& buf);

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

  void setHighlight(BufferPoint from, BufferPoint to) { _highlight = Highlight{from, to}; }
  void clearHighlight() { _highlight.reset(); }
  std::optional<Highlight> const& highlight() const { return _highlight; }

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
  void deleteLine();
  void adjustCursorToIndent();

  // Region operations.
  std::string extractRegion(BufferPoint from, BufferPoint to) const;
  void deleteRegion(BufferPoint from, BufferPoint to);
  BufferPoint insertText(std::string_view text);

  // --- History mode ---
  // Only meaningful when the underlying buffer is a TextBuffer; no-ops
  // otherwise. Entering history mode flips the buffer to readonly; all
  // edit methods already guard on readonly() and become no-ops. Committing
  // truncates any forward history after the selected version.
  bool inHistoryMode() const { return _historyIndex.has_value(); }
  std::optional<std::size_t> historyIndex() const { return _historyIndex; }
  void enterHistoryMode();
  void stepHistory(int delta);
  void exitHistoryModeCommit();

  // --- Rendering ---
  void ensureVisible(RowIdx rows, ColIdx cols, bool wrap);
  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    BufferRenderArgs const& args = {}) const;
  std::size_t screenRow(ColIdx cols, bool wrap = true) const;
  bool currentLineWraps(ColIdx cols) const;

  /// Clamp point/mark to valid buffer positions after external edits.
  /// Call this when another view may have modified the buffer's content.
  void clampToBuffer();

  /// Save this view's cursor/viewport into the buffer's saved-state slot.
  /// Called when this is the last view before destruction, to preserve
  /// cursor position for later restoration via fromSavedState().
  void saveViewState();

private:
  /// Cached visual column of the point. Avoids re-scanning the line on
  /// every movement command.
  struct PointColCache {
    std::size_t line = SIZE_MAX;
    std::size_t offset = 0;
    std::size_t visualCol = 0;
    std::size_t codepointCol = 0;
  };

  /// Cached visual-row boundaries for a wrapped line. Rebuilt when the
  /// line index or column count changes.
  struct RowBoundsCache {
    std::size_t lineIdx = SIZE_MAX;
    std::size_t cols = 0;
    std::vector<std::size_t> rowStarts;
  };

  /// Visual column of the point from byte 0, cached.
  std::size_t pointVisualCol() const;

  /// Ensure the row bounds cache is populated for the given line and cols.
  /// Mirrors Buffer::ensureRowBounds() but reads line data through
  /// _buf.lineContent().
  std::vector<std::size_t> const& ensureRowBounds(std::size_t lineIdx, std::size_t cols) const;

  /// Line index that is `lines` lines before the end of the buffer.
  std::size_t viewFromLinesBeforeEnd(std::size_t lines) const;

  /// True if point is at the very end of the buffer (last line, past all
  /// chars).
  bool atEnd() const;

  Buffer& _buf;
  BufferPoint _point;
  std::optional<BufferPoint> _mark;
  ViewFrom _viewFrom;
  bool _wrapMode = false;
  std::optional<ColIdx> _desiredCol;
  std::optional<Highlight> _highlight;

  mutable PointColCache _pointColCache;
  mutable RowBoundsCache _rowBoundsCache;

  std::size_t _docVersion = 0;
  std::optional<std::size_t> _historyIndex;
};
