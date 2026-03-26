#pragma once

#include "modes/c/syntax.hpp"
#include "types.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/// Buffer mode determines file-type-specific behaviour such as syntax
/// highlighting, auto-indent, and formatting.
enum class BufferMode { Txt, Cpp, Md };

// Abstract base for all buffer types. Provides text storage, a point
// (insertion cursor), navigation, and rendering. Editing operations are
// guarded by readonly() so that derived classes can forbid mutation.
//
// Text is stored as one std::string per line, without '\n' delimiters.
// An empty buffer has _lines = {{}} (one empty line). A file ending with
// '\n' is represented by an empty trailing entry.
class Buffer {
public:
  virtual ~Buffer() = default;

  Buffer(Buffer const&) = delete;
  Buffer& operator=(Buffer const&) = delete;
  Buffer(Buffer&&) = delete;
  Buffer& operator=(Buffer&&) = delete;

  /// Pure virtual — true for readonly buffers (HelpBuffer, BufferListBuffer).
  /// All mutation methods (insert, deleteAtPoint, deleteBeforePoint) are
  /// no-ops when this returns true.
  virtual bool readonly() const = 0;

  /// True after any edit since last save (or creation). TextBuffer clears
  /// this flag on successful save().
  bool modified() const { return _modified; }

  /// Whether this buffer is backed by a file on disk. Default: false.
  /// Overridden by TextBuffer.
  virtual bool hasPath() const { return false; }

  /// The buffer's current mode. Determines syntax highlighting,
  /// auto-indent, and formatting behaviour.
  BufferMode mode() const { return _mode; }
  /// Switch the buffer mode. Invalidates the per-line syntax cache so
  /// the next render() recomputes tokenisation from scratch. Callers
  /// are responsible for triggering a redraw afterwards.
  void setMode(BufferMode m) {
    _mode = m;
    _synCacheValidUpto = 0;
  }

  /// Write buffer contents to its file. Returns an empty expected on success,
  /// or an unexpected string describing the error. Default returns an error
  /// (unsaved/readonly buffers). Overridden by TextBuffer.
  virtual std::expected<void, std::string> save() { return std::unexpected("buffer has no path"); }

  /// Reload buffer contents from its backing file. Default returns an error.
  /// Overridden by TextBuffer.
  virtual std::expected<void, std::string> revert() {
    return std::unexpected("buffer has no path");
  }

  /// Run an external formatter on the buffer contents and replace the buffer
  /// text with the result. In Cpp mode, invokes clang-format. If lineRange
  /// is set, only the given 1-based line range is formatted. Returns a
  /// human-readable status string describing the outcome.
  std::string format(std::optional<std::pair<std::size_t, std::size_t>> lineRange = {});

  /// Handle a Tab keypress. In Cpp mode, formats the current line and
  /// adjusts the cursor; otherwise inserts a literal tab. Returns a
  /// status string (empty if no message is needed).
  std::string receiveTab();

  /// Insert a Unicode code point at _point, encoding it as UTF-8. No-op
  /// if readonly(). The entire multi-byte sequence is inserted atomically.
  void insert(uint32_t cp) {
    if (!readonly())
      doInsert(cp);
  }

  /// Delete the character (full UTF-8 sequence) at _point. No-op if
  /// readonly() or point is at end of buffer.
  void deleteAtPoint() {
    if (!readonly())
      doDeleteAtPoint();
  }

  /// Delete the character (full UTF-8 sequence) before _point. No-op if
  /// readonly() or point is at start of buffer. Walks back over
  /// continuation bytes.
  void deleteBeforePoint() {
    if (!readonly())
      doDeleteBeforePoint();
  }

  /// Delete forward from _point through a contiguous run of same-class
  /// characters. "Blank" = space/tab/newline; everything else is "non-blank".
  /// If the character at point is blank, deletes all blanks forward until a
  /// non-blank (or end of buffer). Otherwise deletes non-blanks forward until
  /// a blank (or end of buffer). No-op if readonly() or at end of buffer.
  void deleteChunkAtPoint() {
    if (!readonly())
      doDeleteChunkAtPoint();
  }

  /// Delete backward from _point through a contiguous run of same-class
  /// characters. Mirrors deleteChunkAtPoint but operates in reverse.
  /// No-op if readonly() or at start of buffer.
  void deleteChunkBeforePoint() {
    if (!readonly())
      doDeleteChunkBeforePoint();
  }

  /// Move point one code point to the left. Safe at start (no-op).
  /// Skips back over UTF-8 continuation bytes. Resets _desiredCol.
  void moveLeft();

  /// Move point one code point to the right. Safe at end (no-op).
  /// Advances past the full UTF-8 sequence. Resets _desiredCol.
  void moveRight();

  /// Move point up by one visual (screen) row. Within a wrapped line,
  /// moves to the previous visual row; at the first visual row, crosses
  /// to the last visual row of the previous physical line.
  /// Maintains _desiredCol as a screen column. Requires cols for wrap
  /// boundary computation.
  void moveUp(ColIdx cols);

  /// Move point down by one visual (screen) row. Within a wrapped line,
  /// moves to the next visual row; at the last visual row, crosses to
  /// the first visual row of the next physical line.
  void moveDown(ColIdx cols);

  /// Move point to the same visual column on the previous physical line.
  /// Maintains _desiredCol. Up from line 1 moves to position 0.
  void movePhysicalUp();

  /// Move point to the same visual column on the next physical line.
  /// Maintains _desiredCol. Down from the last line moves to end of buffer.
  void movePhysicalDown();

  /// Move point to the start of the current line. Resets _desiredCol.
  void moveHome();

  /// Move point to the end of the current line (before '\n').
  /// Resets _desiredCol.
  void moveEnd();

  /// Move point to the right end (last char) of the current word, or if
  /// already there, to the right end of the next word. A "word" is any
  /// run of non-separator characters; separators are spaces, tabs, and
  /// newlines. Resets _desiredCol.
  void moveWordRight();

  /// Move point to the left end (first char) of the current word, or if
  /// already there, to the left end of the previous word. Resets _desiredCol.
  void moveWordLeft();

  /// Move point up by one paragraph: scan upward for the first blank line,
  /// then continue upward to the first non-blank line above it (the last
  /// line of the previous paragraph), then find the start of that paragraph.
  /// Blank = empty or only spaces/tabs. Resets _desiredCol.
  void moveParagraphUp();

  /// Move point down by one paragraph: skip non-blank lines (rest of
  /// current paragraph), then blank lines, and land on the first line of
  /// the next paragraph. Resets _desiredCol.
  void moveParagraphDown();

  /// Move point up by half a screen. If already within the first
  /// half-screen of the file, moves to the start instead. Also scrolls
  /// the viewport by the same amount so the cursor stays at roughly the
  /// same screen position.
  void pageUp(RowIdx screenRows, ColIdx cols, bool wrap);

  /// Move point down by half a screen. If already within the last
  /// half-screen of the file, moves to the end instead. Also scrolls
  /// the viewport by the same amount so the cursor stays at roughly the
  /// same screen position.
  void pageDown(RowIdx screenRows, ColIdx cols, bool wrap);

  /// If point is on a bracket character, move to the matching bracket.
  /// Returns true if the jump was performed.
  bool gotoMatchingBracket();

  /// Move point to the start of the given 1-based line number and center
  /// _viewFromLine so the line appears in the middle of the screen.
  /// Line 0 is treated as line 1; past-end clamps to the last line.
  void gotoLine(std::size_t line, RowIdx screenRows);

  /// Advance _viewFromLine to the next line. Safe at end (stays put).
  void scrollDown();

  /// Move _viewFrom back one line. In wrap mode, first clears the sub-line
  /// col offset if non-zero (go to start of current physical line) before
  /// decrementing the line. Safe at start (no-op).
  void scrollUp(bool wrap = false);

  /// Scroll _viewFromLine down by one line when the cursor is at the end of
  /// the file, but not past the position half a screen before EOF.
  /// This lets the user push the last line toward the screen centre.
  void scrollViewNearEnd(RowIdx screenRows);

  /// True if the line containing the point occupies more than one visual
  /// row at the given column width. Always false when cols <= 1.
  bool currentLineWraps(ColIdx cols) const;

  /// Compute the 0-indexed screen row of the point relative to _viewFromLine,
  /// given a screen column width of cols. cols.value must be > 0.
  /// Accounts for line wrapping.
  virtual std::size_t screenRow(ColIdx cols, bool wrap = true) const;

  /// Scroll the viewport down just enough so the point is visible on
  /// screen (within rows). Jumps _viewFromLine directly when the point
  /// is far off-screen, avoiding the O(N²) cost of one-line-at-a-time
  /// scrolling followed by screenRow() recomputation.
  void ensureVisible(RowIdx rows, ColIdx cols, bool wrap);

  /// Append a rows x cols screen image (with ANSI escapes) to `out`,
  /// starting from _viewFromLine. rows and cols must be > 0. Returns the
  /// screen {row, col} of the point for cursor positioning.
  virtual std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                            bool showWhitespace = false, bool wrap = true) const;

  // Mark
  std::optional<BufferPoint> mark() const { return _mark; }
  void setMark(BufferPoint p) { _mark = p; }
  void clearMark() { _mark.reset(); }
  void toggleMark(); // set at _point if unset, else clear

  // Region operations (normalize from/to internally)
  std::string extractRegion(BufferPoint from, BufferPoint to) const;
  void deleteRegion(BufferPoint from, BufferPoint to); // moves _point to min(from,to)
  BufferPoint insertText(std::string_view text);       // returns point after insertion

  BufferPoint point() const { return _point; }
  // Returns the viewport origin as a BufferPoint. In wrap mode the offset
  // field is the byte offset into the first visible line (sub-line scroll);
  // in no-wrap mode it is 0 (horizontal scroll is kept in ViewFrom::col but
  // is not part of the cursor-position comparison).
  BufferPoint viewFrom() const;
  // Direct access to the viewport struct for callers that need both fields.
  ViewFrom const& viewFromRaw() const { return _viewFrom; }
  std::string const& name() const { return _name; }

  /// Return the 1-indexed {line, column} of the point. Column counts
  /// code points, not visual width or bytes.
  std::tuple<std::size_t, std::size_t> lineCol() const;

  /// Number of lines in the buffer.
  std::size_t lineCount() const { return _lines.size(); }

  /// Access the raw content of a line by 0-based index.
  std::string const& lineContent(std::size_t idx) const { return _lines[idx]; }

  /// Set point to (line, offset). Clamps to valid range. Resets _desiredCol.
  void setPoint(std::size_t line, std::size_t offset);

  /// Set a temporary highlight range (used by find). render() will display
  /// characters in [from, to) with a highlight style.
  void setHighlight(BufferPoint from, BufferPoint to) { _highlight = Highlight{from, to}; }
  void clearHighlight() { _highlight.reset(); }

protected:
  explicit Buffer(std::string name);

  void doInsert(uint32_t cp);
  void doDeleteAtPoint();
  void doDeleteBeforePoint();
  void doDeleteChunkAtPoint();
  void doDeleteChunkBeforePoint();
  // Sum of display widths of characters in line data[from..to).
  std::size_t visualCol(std::string const& line, std::size_t from, std::size_t to) const;
  // True if line idx is empty or contains only spaces/tabs.
  bool isBlankLine(std::size_t idx) const;
  // Line index that is `lines` lines before the end of the buffer.
  std::size_t viewFromLinesBeforeEnd(std::size_t lines) const;

  // True if point is at the very end of the buffer (last line, past all chars).
  bool atEnd() const {
    return _point.line == _lines.size() - 1 && _point.offset == _lines.back().size();
  }

  // Visual column of the point from byte 0, cached. All edits change
  // _point, so {line, offset} is a valid cache key.
  std::size_t pointVisualCol() const;

  std::string _name;
  BufferMode _mode = BufferMode::Txt;
  std::vector<std::string> _lines{{}};
  BufferPoint _point;
  std::optional<BufferPoint> _mark;
  ViewFrom _viewFrom;
  bool _wrapMode = false; // tracks last wrap mode seen by ensureVisible
  std::optional<ColIdx> _desiredCol;
  bool _modified = false;

  // Memoisation cache for point column computations. Both lineCol() and
  // pointVisualCol() scan from byte 0 to _point.offset on the current
  // line; this cache ensures the scan happens at most once per point
  // position.
  struct PointColCache {
    std::size_t line = SIZE_MAX;
    std::size_t offset = SIZE_MAX;
    std::size_t visualCol = 0;
    std::size_t codepointCol = 0;
  };
  mutable PointColCache _pointColCache;

  struct Highlight {
    BufferPoint from;
    BufferPoint to;
  };
  std::optional<Highlight> _highlight;

  // Cached visual-row boundary offsets for a single line. Populated on
  // first findVisualRow() call for that line+cols combination; invalidated
  // by edits (which set lineIdx = SIZE_MAX).
  struct RowBoundsCache {
    std::size_t lineIdx = SIZE_MAX;
    std::size_t cols = 0;
    std::vector<std::size_t> rowStarts; // byte offset of each visual row
  };
  mutable RowBoundsCache _rowBoundsCache;
  // Ensure the row bounds cache is populated for the given line and cols.
  // Returns a const reference to _rowBoundsCache.rowStarts.
  std::vector<std::size_t> const& ensureRowBounds(std::size_t lineIdx, std::size_t cols) const;

  // --- Per-line syntax cache ---
  // Populated lazily by render() for Cpp-mode buffers. Each entry stores
  // the tokenisation result and the multi-line state at the *end* of the
  // line (i.e. the state that feeds into the *next* line's tokenisation).
  // Lines [0, _synCacheValidUpto) have valid entries.
  struct LineSyntaxCache {
    SyntaxState stateAfter;          // state at end of this line
    std::vector<SyntaxToken> tokens; // per-byte token classification
  };
  mutable std::vector<LineSyntaxCache> _synCache;
  mutable std::size_t _synCacheValidUpto = 0;

  // Mark all cache entries from `fromLine` onwards as stale.
  void invalidateSynCache(std::size_t fromLine);
  // Ensure the cache covers lines [0, upToLine).  Called by render().
  void ensureSynCache(std::size_t upToLine) const;
};
