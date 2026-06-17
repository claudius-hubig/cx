#pragma once

/**
 * @file buffer_view.hpp
 * @brief BufferView — all per-view state and behaviour for one logical
 *        view of a Buffer: cursor, mark, viewport, movement, editing, and
 *        rendering.
 *
 * Multiple BufferViews may share one Buffer for split-window editing; each
 * keeps independent cursor/viewport state and re-clamps itself when it
 * notices the buffer changed underneath it.
 */

#include "buffer.hpp"
#include "types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/**
 * @brief Display inputs for BufferView::render() that come from the
 *        surrounding panel/screen rather than the view itself.
 *
 * Per-view flags such as @c showWhitespace and @c wrap live on BufferView;
 * this struct carries only focus and panel-rect offsets.
 */
struct BufferRenderArgs {
  bool focused = true;       /**< Whether this panel has focus. */
  std::size_t rowOffset = 0; /**< Screen row at which the panel starts. */
  std::size_t colOffset = 0; /**< Screen column at which the panel starts. */
};

/**
 * @brief Per-view state and behaviour for a Buffer.
 *
 * Owns the cursor, selection mark, viewport scroll, wrap mode, desired
 * column, find highlight, and per-view caches. Movement, editing, and
 * rendering are implemented on top of Buffer's public accessors and its
 * position-explicit editing API.
 *
 * Multiple BufferViews can reference the same Buffer; each maintains
 * independent cursor/viewport state and detects external edits via
 * Buffer::docVersion(), clamping its positions on the next access.
 */
class BufferView {
public:
  /** @brief Construct a view at position (0,0) for @p buf. */
  explicit BufferView(Buffer& buf);

  /**
   * @brief Construct a view from the buffer's saved view state, if any.
   *
   * Starts at (0,0) when the buffer has no saved state. Clears the saved
   * state after reading it.
   */
  static BufferView fromSavedState(Buffer& buf);

  /** @brief The buffer this view observes. */
  Buffer& buffer() { return _buf; }
  /** @brief The buffer this view observes (const). */
  Buffer const& buffer() const { return _buf; }

  /**
   * @name View-state accessors
   */
  /** @{ */
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
  /** @} */

  /**
   * @brief Per-view soft-wrap mode (false = horizontal scroll, true = wrap).
   *
   * Toggling clears the horizontal/sub-line scroll state because the
   * ViewFrom variant alternative changes.
   */
  bool wrap() const { return _wrap; }
  void setWrap(bool w);
  void toggleWrap() { setWrap(!_wrap); }

  /**
   * @brief Per-view whitespace-marker visibility (spaces, tabs, control
   *        bytes).
   *
   * A pure render flag; does not affect editing or storage.
   */
  bool showWhitespace() const { return _showWhitespace; }
  void setShowWhitespace(bool s) { _showWhitespace = s; }
  void toggleShowWhitespace() { _showWhitespace = !_showWhitespace; }

  /**
   * @brief Per-view current-line highlight.
   *
   * When on, the cursor's physical line gets a subtle background tint;
   * lines that already carry a diff/conflict background keep theirs.
   */
  bool highlightCurrentLine() const { return _highlightCurrentLine; }
  void setHighlightCurrentLine(bool h) { _highlightCurrentLine = h; }
  void toggleHighlightCurrentLine() { _highlightCurrentLine = !_highlightCurrentLine; }

  /**
   * @brief Per-view line-number gutter.
   *
   * When on, render() reserves columns at the panel's left edge for
   * right-aligned 1-based line numbers; the width derives from the
   * buffer's line count plus a trailing separator space.
   */
  bool showLineNumbers() const { return _showLineNumbers; }
  void setShowLineNumbers(bool s) { _showLineNumbers = s; }
  void toggleShowLineNumbers() { _showLineNumbers = !_showLineNumbers; }
  /** @brief Width of the left gutter in screen columns (0 when off). */
  std::size_t lineNumberGutterWidth() const;

  /** @brief Current cursor position as a 1-based {line, column} pair. */
  std::tuple<std::size_t, std::size_t> lineCol() const;

  /**
   * @name Movement
   *
   * Wrap-sensitive methods read the per-view wrap() state. The explicit
   * @c wrap parameters on the lower-level helpers remain so call sites can
   * ask "what would happen at a given wrap setting".
   */
  /** @{ */
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

  /**
   * @brief Scroll so the cursor sits at a chosen vertical screen position.
   *
   * Wrap-aware: walks visual rows in wrap mode, physical lines otherwise.
   * Used by the centre-cursor (Ctrl-l) action.
   * @param screenRows Panel content height.
   * @param position 0-indexed target row within that height (clamped).
   * @param cols Panel width (used for visual-row counting in wrap mode).
   * @param wrap Whether the view is in line-wrap mode.
   */
  void recenter(RowIdx screenRows, std::size_t position, ColIdx cols, bool wrap);
  /** @} */

  /**
   * @name Editing
   */
  /** @{ */
  void insert(uint32_t cp);
  void deleteAtPoint();
  void deleteBeforePoint();
  void deleteChunkAtPoint();
  void deleteChunkBeforePoint();
  void deleteLine();

  /**
   * @brief Toggle a line-comment prefix on the lines under the active
   *        region (or the current line if no mark is set).
   *
   * If every non-blank line in the range already begins with @p prefix
   * (after its leading whitespace), the prefix is removed; otherwise
   * @p prefix is prepended at column 0 to every non-blank line. Blank
   * lines are preserved, so add-then-remove round-trips. The prefix is
   * mode-defined by the caller (e.g. "// " for C/C++).
   */
  void toggleComment(std::string_view prefix);

  /**
   * @brief Reflow the paragraph at the cursor (or the lines under the
   *        active region) to fit @p targetWidth visual columns.
   *
   * The common leading-whitespace prefix is preserved on every output
   * line; a recognised list bullet on the first input line
   * (`- `, `* `, `+ `, `1. `, `1) `) is preserved and continuation lines
   * are indented under it so list structure round-trips. Words split on
   * whitespace; UTF-8 widths follow the renderer's visualWidth rules.
   * A no-op on a blank cursor line with no active region.
   */
  void reflowParagraph(std::size_t targetWidth);

  /** @brief Advance the cursor to the first non-space byte if it sits in
   *         the leading whitespace. */
  void adjustCursorToIndent();

  std::string extractRegion(BufferPoint from, BufferPoint to) const;
  void deleteRegion(BufferPoint from, BufferPoint to);
  BufferPoint insertText(std::string_view text);
  /** @} */

  /**
   * @name History mode
   *
   * Meaningful only when the underlying buffer is a TextBuffer; no-ops
   * otherwise. Entering flips the buffer to read-only (so all edit methods
   * become no-ops via their readonly() guard); committing truncates any
   * forward history after the selected version.
   */
  /** @{ */
  bool inHistoryMode() const { return _historyIndex.has_value(); }
  std::optional<std::size_t> historyIndex() const { return _historyIndex; }
  void enterHistoryMode();
  void stepHistory(int delta);
  void exitHistoryModeCommit();
  /** @} */

  /**
   * @name Rendering
   */
  /** @{ */
  void ensureVisible(RowIdx rows, ColIdx cols, bool wrap);
  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    BufferRenderArgs const& args = {}) const;
  std::size_t screenRow(ColIdx cols, bool wrap = true) const;
  bool currentLineWraps(ColIdx cols) const;

  /**
   * @brief Inverse of screenRow: the buffer point at a screen cell.
   *
   * Given a 0-based screen position relative to the viewport (@p screenRow
   * rows down, @p screenCol columns from the content area's left edge),
   * return the buffer point at that visual cell, clamped to a valid
   * position. Used by Screen to translate mouse clicks into cursor
   * positions.
   */
  BufferPoint pointForScreenRowCol(std::size_t screenRow, std::size_t screenCol, ColIdx cols,
                                   bool wrap) const;
  /** @} */

  /**
   * @brief Re-clamp point/mark/viewport to valid positions after external
   *        edits, drop the desired-column memory, and invalidate caches.
   *
   * Invoked automatically by public methods on a docVersion mismatch;
   * exposed so Screen can call it explicitly after large buffer rebuilds
   * (e.g. a buffer-list refresh).
   */
  void clampToBuffer();

  /**
   * @brief Persist this view's state to the buffer's saved-state slot.
   *
   * Lets a later view built via fromSavedState() restore point, viewFrom,
   * mark, desired column, and wrap mode. Called whenever a panel is about
   * to drop its BufferView — buffer switch or panel close — not just at
   * last-view destruction.
   */
  void saveViewState();

private:
  /**
   * @brief Cached visual column of the point, to avoid re-scanning the
   *        line on every movement command.
   */
  struct PointColCache {
    std::size_t line = SIZE_MAX;
    std::size_t offset = 0;
    std::size_t visualCol = 0;
    std::size_t codepointCol = 0;
  };

  /**
   * @brief Cached visual-row boundaries for a wrapped line; rebuilt when
   *        the line index or column count changes.
   */
  struct RowBoundsCache {
    std::size_t lineIdx = SIZE_MAX;
    std::size_t cols = 0;
    std::vector<std::size_t> rowStarts;
  };

  /**
   * @brief Effective content width inside a panel of @p cols total
   *        columns: total minus the right gutter and any left line-number
   *        gutter.
   */
  std::size_t contentCols(ColIdx cols) const;

  /** @brief Visual column of the point from byte 0, cached. */
  std::size_t pointVisualCol() const;

  /**
   * @brief Ensure the row-bounds cache is populated for @p lineIdx at
   *        @p cols columns.
   */
  std::vector<std::size_t> const& ensureRowBounds(std::size_t lineIdx, std::size_t cols) const;

  /** @brief Line index that is @p lines lines before the end of the buffer. */
  std::size_t viewFromLinesBeforeEnd(std::size_t lines) const;

  /** @brief True if the point is at the very end of the buffer. */
  bool atEnd() const;

  Buffer& _buf;
  BufferPoint _point;
  std::optional<BufferPoint> _mark;
  ViewFrom _viewFrom;
  /**
   * @brief Soft-wrap mode (source of truth). @c _wrapMode mirrors it for
   *        ensureVisible's "did wrap mode change?" detection.
   */
  bool _wrap = false;
  bool _wrapMode = false;
  bool _showWhitespace = false;
  bool _highlightCurrentLine = false;
  bool _showLineNumbers = false;
  std::optional<ColIdx> _desiredCol;
  std::optional<Highlight> _highlight;

  mutable PointColCache _pointColCache;
  mutable RowBoundsCache _rowBoundsCache;

  /** @brief Position used by ensureVisible's forward scan. */
  struct ViewPos {
    std::size_t line;
    std::size_t byteOff;
  };
  /**
   * @brief Scratch ring buffer for ensureVisible's forward scan, kept as a
   *        member so the per-call resize is in-place.
   */
  mutable std::vector<ViewPos> _ensureVisibleRing;

  std::size_t _docVersion = 0;
  std::optional<std::size_t> _historyIndex;
};
