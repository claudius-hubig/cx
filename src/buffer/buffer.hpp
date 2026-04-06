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
enum class BufferMode { Txt, Cpp, Md, Jsn };

/// A search or selection highlight range. Defined outside Buffer so that
/// RenderParams can reference it without depending on Buffer's definition.
struct Highlight {
  BufferPoint from;
  BufferPoint to;
};

/// Parameters for rendering a buffer view. Passed to render() so the
/// renderer doesn't need to read view state from the buffer directly.
struct RenderParams {
  BufferPoint point;
  ViewFrom viewFrom;
  std::optional<BufferPoint> mark;
  std::optional<Highlight> highlight;
  bool showWhitespace = false;
  bool wrap = true;
  std::size_t rowOffset = 0;
  std::size_t colOffset = 0;
};

// Abstract base for all buffer types. Owns document content only: line
// storage, filename, mode, modified flag, document version, and syntax
// cache. All view state (cursor position, viewport, mark, selection,
// per-view caches) lives in BufferView. Multiple BufferViews can share
// the same Buffer for split-window editing.
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
  /// BufferView's editing methods are no-ops when this returns true.
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

  std::string const& name() const { return _name; }

  /// Number of lines in the buffer.
  std::size_t lineCount() const { return _lines.size(); }

  /// Access the raw content of a line by 0-based index.
  std::string const& lineContent(std::size_t idx) const { return _lines[idx]; }

  /// True if line idx is empty or contains only spaces/tabs.
  bool isBlankLine(std::size_t idx) const;

  /// Direct const access to the line storage.
  std::vector<std::string> const& lines() const { return _lines; }

  /// Document version counter, incremented on every edit. BufferView uses
  /// this to detect external modifications and invalidate its caches.
  std::size_t docVersion() const { return _docVersion; }

  // --- Position-explicit editing ---
  // These methods operate on the document at an explicit position, returning
  // the new cursor position. They do not read or maintain any view state;
  // callers (typically BufferView) own and update cursor/mark.

  /// Insert a Unicode code point at `at`, encoding it as UTF-8. Newlines
  /// split the current line; for Cpp/Jsn mode, the new line is auto-indented
  /// to match the previous line's leading spaces. Returns the position
  /// immediately after the inserted content.
  BufferPoint insertAt(uint32_t cp, BufferPoint at);

  /// Delete the character (full UTF-8 sequence) at `at`. If `at` is at
  /// end-of-line, joins with the next line. Returns the unchanged position.
  BufferPoint deleteAt(BufferPoint at);

  /// Delete the character before `at`. If `at` is at column 0, joins with
  /// the previous line. Returns the new position (which may be on a
  /// different line after a join).
  BufferPoint deleteBefore(BufferPoint at);

  /// Insert a block of text (possibly containing newlines) at `at`.
  /// Returns the position immediately after the inserted content.
  BufferPoint insertText(std::string_view text, BufferPoint at);

  /// Delete the half-open region [from, to). Arguments are normalised
  /// internally. Does not update any cursor; the caller is responsible.
  void deleteRegion(BufferPoint from, BufferPoint to);

  /// Extract the text of the half-open region [from, to) as a string,
  /// with '\n' between lines. Arguments are normalised internally.
  std::string extractRegion(BufferPoint from, BufferPoint to) const;

  // --- Rendering ---

  /// Compute the 0-indexed screen row of the given point relative to
  /// viewFrom, given a screen column width of cols. Accounts for line
  /// wrapping when wrap=true.
  virtual std::size_t screenRow(ColIdx cols, BufferPoint point, ViewFrom viewFrom,
                                bool wrap = true) const;

  /// Append a rows x cols screen image (with ANSI escapes) to `out`,
  /// using the view state supplied in `params`. Returns the screen
  /// {row, col} of the point for cursor positioning.
  virtual std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                            RenderParams const& params) const;

  // --- Saved view state for buffer switching ---
  // When a panel switches away from this buffer, it saves minimal cursor
  // and viewport state here so a new BufferView can restore it later.
  struct SavedViewState {
    BufferPoint point;
    ViewFrom viewFrom;
    std::optional<BufferPoint> mark;
  };
  void setSavedView(SavedViewState s) { _savedView = s; }
  std::optional<SavedViewState> savedView() const { return _savedView; }
  void clearSavedView() { _savedView.reset(); }

protected:
  explicit Buffer(std::string name);

  std::string _name;
  BufferMode _mode = BufferMode::Txt;
  std::vector<std::string> _lines{{}};
  bool _modified = false;
  std::size_t _docVersion = 0; // incremented on every edit
  std::optional<SavedViewState> _savedView;

  // --- Per-line syntax cache ---
  // Populated lazily by render() for modes with syntax highlighting. Each
  // entry stores the tokenisation result and the multi-line state at the
  // *end* of the line (i.e. the state that feeds into the *next* line's
  // tokenisation). Lines [0, _synCacheValidUpto) have valid entries.
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
