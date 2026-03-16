#pragma once

#include "types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Abstract base for all buffer types. Provides text storage, a point
// (insertion cursor), navigation, and rendering. Editing operations are
// guarded by readonly() so that derived classes can forbid mutation.
class Buffer {
public:
  virtual ~Buffer() = default;

  Buffer(Buffer const &) = delete;
  Buffer &operator=(Buffer const &) = delete;
  Buffer(Buffer &&) = delete;
  Buffer &operator=(Buffer &&) = delete;

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

  /// Write buffer contents to its file. Returns true on success.
  /// Default returns false (unsaved/readonly buffers). Overridden by
  /// TextBuffer.
  virtual bool save() { return false; }

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

  /// Move point one code point to the left. Safe at start (no-op).
  /// Skips back over UTF-8 continuation bytes. Resets _desiredCol.
  void moveLeft();

  /// Move point one code point to the right. Safe at end (no-op).
  /// Advances past the full UTF-8 sequence. Resets _desiredCol.
  void moveRight();

  /// Move point to the same visual column on the previous line.
  /// Maintains _desiredCol for vertical movement across lines of
  /// different lengths. Up from line 1 moves to position 0.
  void moveUp();

  /// Move point to the same visual column on the next line.
  /// Maintains _desiredCol for vertical movement. Down from the last
  /// line moves to end of buffer.
  void moveDown();

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

  /// Move point to the start of the given 1-based line number and center
  /// _viewFrom so the line appears in the middle of the screen.
  /// Line 0 is treated as line 1; past-end clamps to the last line.
  void gotoLine(std::size_t line, RowIdx screenRows);

  /// Advance _viewFrom to the start of the next line. Safe at end
  /// (stays put if no newline found).
  void scrollDown();

  /// Move _viewFrom back to the start of the previous line. Safe at
  /// start (_viewFrom == 0 is a no-op).
  void scrollUp();

  /// Compute the 0-indexed screen row of the point relative to _viewFrom,
  /// given a screen column width of cols. cols.value must be > 0.
  /// Accounts for line wrapping.
  virtual std::size_t screenRow(ColIdx cols) const;

  /// Append a rows x cols screen image (with ANSI escapes) to `out`,
  /// starting from _viewFrom. rows and cols must be > 0. Returns the
  /// screen {row, col} of the point for cursor positioning.
  virtual std::tuple<RowIdx, ColIdx> render(std::string &out, RowIdx rows,
                                            ColIdx cols,
                                            bool showWhitespace = false) const;

  BufferPoint point() const { return _point; }
  BufferPoint viewFrom() const { return _viewFrom; }
  std::string const &name() const { return _name; }

  /// Return the 1-indexed {line, column} of the point. Column counts
  /// code points, not visual width or bytes.
  std::tuple<std::size_t, std::size_t> lineCol() const;

protected:
  explicit Buffer(std::string name);

  void doInsert(uint32_t cp);
  void doDeleteAtPoint();
  void doDeleteBeforePoint();
  // Index of the first byte of the line containing pos (scans back for '\n').
  std::size_t lineStart(std::size_t pos) const;
  // Sum of display widths of characters in _data[from..to).
  std::size_t visualCol(std::size_t from, std::size_t to) const;
  // True if the line starting at ls is empty or contains only spaces/tabs.
  bool isBlankLine(std::size_t ls) const;

  std::string _name;
  std::vector<char> _data;
  BufferPoint _point = 0;
  BufferPoint _viewFrom = 0;
  std::optional<ColIdx> _desiredCol;
  bool _modified = false;
};
