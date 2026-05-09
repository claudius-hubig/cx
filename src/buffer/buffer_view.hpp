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

/// Display settings passed to BufferView::render(). `showWhitespace`
/// and `wrap` are per-view state on `BufferView` itself; this struct
/// only carries the inputs that come from the surrounding panel /
/// screen (focus, panel rect offsets).
struct BufferRenderArgs {
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

  /// Per-view soft-wrap mode. False = no wrap (horizontal scroll); true
  /// = wrap visual lines. Toggling clears the horizontal/sub-line scroll
  /// state because the variant alternative changes.
  bool wrap() const { return _wrap; }
  void setWrap(bool w);
  void toggleWrap() { setWrap(!_wrap); }

  /// Per-view whitespace-marker visibility (spaces · , tabs ▶, control
  /// bytes). Pure render flag — does not affect editing or storage.
  bool showWhitespace() const { return _showWhitespace; }
  void setShowWhitespace(bool s) { _showWhitespace = s; }
  void toggleShowWhitespace() { _showWhitespace = !_showWhitespace; }

  /// Per-view current-line highlight: when on, the cursor's physical
  /// line is rendered with a subtle background tint (lines that already
  /// carry a diff or conflict background keep that background instead).
  bool highlightCurrentLine() const { return _highlightCurrentLine; }
  void setHighlightCurrentLine(bool h) { _highlightCurrentLine = h; }
  void toggleHighlightCurrentLine() { _highlightCurrentLine = !_highlightCurrentLine; }

  std::tuple<std::size_t, std::size_t> lineCol() const;

  // --- Movement ---
  // Methods that are wrap-mode-sensitive read the per-view `wrap()`
  // state. `wrap` parameters on the lower-level helpers (pageUp, scrollUp,
  // pointForScreenRowCol, ensureVisible, screenRow) remain because some
  // call sites need to ask "what would happen at a given wrap setting".
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

  /// Scroll so the cursor sits at a chosen vertical position on the
  /// screen. `screenRows` is the panel's content height; `position` is
  /// the 0-indexed target row within that height (clamped). Wrap-aware:
  /// in wrap mode, walks visual rows; in no-wrap mode, physical lines.
  /// Used by the centre-cursor (Ctrl-l) action.
  void recenter(RowIdx screenRows, std::size_t position, ColIdx cols, bool wrap);

  // --- Editing ---
  void insert(uint32_t cp);
  void deleteAtPoint();
  void deleteBeforePoint();
  void deleteChunkAtPoint();
  void deleteChunkBeforePoint();
  void deleteLine();

  // Toggle a line-comment prefix on the lines covered by the active
  // mark/point region (or the current line if no mark is set). If every
  // non-blank line in the range already begins with `prefix` (after its
  // leading whitespace), the prefix is removed; otherwise `prefix` is
  // prepended at column 0 to every non-blank line. The prefix is
  // mode-defined by the caller (e.g. "// " for C/C++). Blank lines are
  // preserved either way so adding then removing comments round-trips.
  void toggleComment(std::string_view prefix);

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

  /// Inverse of screenRow: given a 0-based screen position relative to
  /// the viewport (`screenRow` rows down, `screenCol` columns from the
  /// content area's left edge), return the buffer point that lives at
  /// that visual cell. The result is clamped to a valid position. Used
  /// by `Screen` to translate mouse clicks into cursor positions.
  BufferPoint pointForScreenRowCol(std::size_t screenRow, std::size_t screenCol, ColIdx cols,
                                   bool wrap) const;

  /// Clamp point/mark/viewport to valid buffer positions after external
  /// edits, drop the desired-column memory, and invalidate per-view
  /// caches. Automatically invoked by all public methods on a
  /// docVersion mismatch; exposed publicly so `Screen` can call it
  /// explicitly after large buffer rebuilds (e.g. a buffer-list refresh).
  void clampToBuffer();

  /// Persist this view's state to the buffer's saved-state slot, so a
  /// later view constructed via `fromSavedState()` can restore it.
  /// Preserved fields: point, viewFrom (variant alternative + value),
  /// mark, desired column, and wrap mode. Called whenever a panel is
  /// about to drop its `BufferView` — buffer switch or panel close —
  /// not just at last-view destruction.
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
  // Soft-wrap mode for this view (source of truth). `_wrapMode` mirrors
  // it for ensureVisible's "did wrap mode change?" detection — kept
  // separate so future call sites that pass an explicit wrap value into
  // ensureVisible (e.g. a temporary preview render) still get the
  // change-detection behaviour.
  bool _wrap = false;
  bool _wrapMode = false;
  bool _showWhitespace = false;
  bool _highlightCurrentLine = false;
  std::optional<ColIdx> _desiredCol;
  std::optional<Highlight> _highlight;

  mutable PointColCache _pointColCache;
  mutable RowBoundsCache _rowBoundsCache;

  /// Scratch ring buffer for ensureVisible's forward scan. Stored as a
  /// member so the per-call resize is in-place rather than a fresh heap
  /// allocation on every cursor move and render.
  struct ViewPos {
    std::size_t line;
    std::size_t byteOff;
  };
  mutable std::vector<ViewPos> _ensureVisibleRing;

  std::size_t _docVersion = 0;
  std::optional<std::size_t> _historyIndex;
};
