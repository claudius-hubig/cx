#pragma once

/**
 * @file buffer.hpp
 * @brief Buffer — the abstract base for every buffer type, owning the
 *        document content (lines, mode, modified flag, version, syntax
 *        cache) while all view state lives in BufferView.
 *
 * Also defines the rendering vocabulary (RenderParams, Highlight) and the
 * BufferMode enum that selects the per-mode syntax tokenizer.
 */

#include "modes/c/syntax.hpp"
#include "types.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/**
 * @brief Selects file-type-specific behaviour: syntax highlighting,
 *        auto-indent, and formatting.
 */
enum class BufferMode { Txt, Cpp, Md, Jsn, Dif, Sh, Yml, Py };

/**
 * @brief True if @p line is a git merge-conflict marker (7 identical
 *        leading '<', '=', or '>' characters).
 */
inline bool isConflictMarker(std::string const& line) {
  if (line.size() < 7)
    return false;
  char c = line[0];
  return (c == '<' || c == '=' || c == '>') && line[1] == c && line[2] == c && line[3] == c &&
         line[4] == c && line[5] == c && line[6] == c;
}

/**
 * @brief A search or selection highlight range.
 *
 * Defined outside Buffer so RenderParams can reference it without
 * depending on Buffer's full definition.
 */
struct Highlight {
  BufferPoint from;
  BufferPoint to;
};

/**
 * @brief View-state inputs for a single render() call.
 *
 * Passed to render() so the renderer never reads view state from the
 * buffer directly — that state belongs to BufferView.
 */
struct RenderParams {
  BufferPoint point;                  /**< Cursor position. */
  ViewFrom viewFrom;                  /**< Viewport anchor. */
  std::optional<BufferPoint> mark;    /**< Selection mark, if active. */
  std::optional<Highlight> highlight; /**< Find highlight, if active. */
  bool showWhitespace = false;        /**< Render whitespace markers. */
  bool wrap = true;                   /**< Soft-wrap long lines. */
  bool focused = true;                /**< Whether this panel has focus. */
  /**
   * @brief Tint the entire physical line containing @c point.
   *
   * Diff / conflict backgrounds take precedence on lines that have one
   * (the diff cue matters more than the cursor cue).
   */
  bool highlightCurrentLine = false;
  /**
   * @brief Width (in screen columns) of the left line-number gutter to
   *        reserve at @c colOffset.
   *
   * When > 0 the renderer emits the 1-based physical-line number
   * (right-aligned, dim, trailing space) at the start of each visual row
   * that begins a new physical line; wrap-continuation rows get a blank
   * gutter. Content shifts right by this width.
   */
  std::size_t leftGutter = 0;
  std::size_t rowOffset = 0; /**< Screen row at which this panel starts. */
  std::size_t colOffset = 0; /**< Screen column at which this panel starts. */
};

/**
 * @brief Abstract base for all buffer types; owns document content only.
 *
 * A Buffer owns line storage, filename, mode, modified flag, document
 * version, and the per-line syntax cache. All view state — cursor, mark,
 * viewport, selection, per-view caches — lives in BufferView, so multiple
 * BufferViews can share one Buffer for split-window editing.
 *
 * Text is stored as one immutable string per line, without '\n'
 * delimiters. An empty buffer has a single empty line; a file ending in
 * '\n' is represented by an empty trailing entry.
 */
class Buffer {
public:
  virtual ~Buffer() = default;

  Buffer(Buffer const&) = delete;
  Buffer& operator=(Buffer const&) = delete;
  Buffer(Buffer&&) = delete;
  Buffer& operator=(Buffer&&) = delete;

  /**
   * @brief Whether this buffer rejects edits.
   *
   * BufferView's editing methods become no-ops when this returns true
   * (read-only overlays, and TextBuffer while browsing history).
   */
  virtual bool readonly() const = 0;

  /**
   * @brief Whether @c RenderParams::leftGutter is honoured by this
   *        buffer's render().
   *
   * The base Buffer::render does honour it; subclasses (HelpBuffer,
   * DirBuffer, BufferListBuffer, PasteRingBuffer) that fully override
   * render() with bespoke layouts return false, so a per-view
   * line-number toggle silently no-ops on those overlays.
   */
  virtual bool supportsLineNumberGutter() const { return true; }

  /**
   * @brief True after any edit since the last save (or creation).
   *
   * TextBuffer clears this flag on a successful save().
   */
  bool modified() const { return _modified; }

  /** @brief Whether this buffer is backed by a file on disk (TextBuffer). */
  virtual bool hasPath() const { return false; }

  /** @brief The buffer's current mode (drives highlighting/indent/format). */
  BufferMode mode() const { return _mode; }
  /** @brief True if the buffer currently contains merge-conflict markers. */
  bool hasConflicts() const { return _hasConflicts; }
  /**
   * @brief Switch the buffer mode.
   *
   * Invalidates the per-line syntax cache so the next render()
   * re-tokenises from scratch; the caller must trigger a redraw.
   */
  void setMode(BufferMode m) {
    _mode = m;
    _synCacheValidUpto = 0;
  }

  /**
   * @brief Write buffer contents to its file.
   *
   * The base implementation reports an error (unsaved/read-only buffers);
   * TextBuffer overrides it.
   * @return An empty expected on success, or an error message.
   */
  virtual std::expected<void, std::string> save() { return std::unexpected("buffer has no path"); }

  /**
   * @brief Reload buffer contents from the backing file.
   *
   * The base implementation reports an error; TextBuffer overrides it.
   */
  virtual std::expected<void, std::string> revert() {
    return std::unexpected("buffer has no path");
  }

  /** @brief The buffer's display/identity name. */
  std::string const& name() const { return _name; }

  /** @brief Number of lines in the buffer. */
  std::size_t lineCount() const { return _lines.size(); }

  /** @brief Raw content of the line at 0-based @p idx. */
  std::string const& lineContent(std::size_t idx) const { return *_lines[idx]; }

  /** @brief True if line @p idx is empty or only spaces/tabs. */
  bool isBlankLine(std::size_t idx) const;

  /**
   * @brief Direct const access to the line storage.
   *
   * Snapshots and the live buffer share these LinePtrs; mutation goes
   * through mutLine(), which detaches when shared, so applying a snapshot
   * is O(N pointer copies) rather than O(N string copies).
   */
  std::vector<LinePtr> const& lines() const { return _lines; }

  /**
   * @brief Document version counter, incremented on every edit.
   *
   * BufferView uses it to detect external modifications and invalidate
   * its caches.
   */
  std::size_t docVersion() const { return _docVersion; }

  /**
   * @name Position-explicit editing
   *
   * These methods operate on the document at an explicit position and
   * return the new cursor position. They never read or maintain view
   * state; the caller (typically BufferView) owns cursor and mark.
   */
  /** @{ */

  /**
   * @brief Insert a Unicode code point at @p at, encoded as UTF-8.
   *
   * A newline splits the current line; for indent-preserving modes
   * (Cpp/Jsn/Py) the new line is auto-indented to match the previous
   * line's leading whitespace.
   * @return The position immediately after the inserted content.
   */
  BufferPoint insertAt(uint32_t cp, BufferPoint at);

  /**
   * @brief Delete the character (full UTF-8 sequence) at @p at.
   *
   * At end-of-line, joins with the next line.
   * @return The unchanged position.
   */
  BufferPoint deleteAt(BufferPoint at);

  /**
   * @brief Delete the character before @p at.
   *
   * At column 0, joins with the previous line.
   * @return The new position (possibly on the previous line after a join).
   */
  BufferPoint deleteBefore(BufferPoint at);

  /**
   * @brief Insert a block of text (possibly containing newlines) at @p at.
   * @return The position immediately after the inserted content.
   */
  BufferPoint insertText(std::string_view text, BufferPoint at);

  /**
   * @brief Delete the half-open region [from, to).
   *
   * Arguments are normalised internally. Does not update any cursor.
   */
  void deleteRegion(BufferPoint from, BufferPoint to);

  /**
   * @brief Extract the text of the half-open region [from, to), with '\n'
   *        between lines.
   *
   * Arguments are normalised internally.
   */
  std::string extractRegion(BufferPoint from, BufferPoint to) const;
  /** @} */

  /**
   * @brief Hook invoked by BufferView after each successful edit.
   *
   * TextBuffer overrides this to append a snapshot to its version
   * history; read-only buffers ignore it.
   *
   * @param cursor Cursor position to record.
   * @param dirtyFrom Index of the first modified line, or SIZE_MAX for a
   *        full-buffer diff. Only `[dirtyFrom, dirtyFrom + max(1, 1 +
   *        lineShift))` is treated as modified; preceding lines must be
   *        bit-identical to the prior snapshot and following lines are
   *        unchanged shifted positions.
   * @param lineShift Net change in line count caused by this edit
   *        (positive when the buffer grew, negative when it shrank).
   */
  virtual void recordVersion(BufferPoint cursor, std::size_t dirtyFrom = SIZE_MAX,
                             int lineShift = 0) {
    (void)cursor;
    (void)dirtyFrom;
    (void)lineShift;
  }

  /**
   * @name History-mode virtual interface
   *
   * Default no-ops for buffers without version history. TextBuffer
   * overrides these so BufferView can drive history browsing through the
   * base class without a concrete dependency on TextBuffer.
   */
  /** @{ */

  /** @brief True while a view is stepping through history (read-only). */
  virtual bool browsingHistory() const { return false; }
  /** @brief Enter or leave read-only history-browsing mode. */
  virtual void setBrowsingHistory(bool) {}

  /** @brief Summary of the version history for status-line display. */
  struct VersionHistoryInfo {
    std::size_t size = 0;
    bool empty = true;
  };
  /** @brief Current version-history summary. */
  virtual VersionHistoryInfo versionHistoryInfo() const { return {}; }

  /**
   * @brief Replace buffer contents with the version at @p idx.
   * @param idx Index of the version to restore.
   * @param[out] cursor Set to the cursor position stored with that version.
   */
  virtual void applyVersionAt(std::size_t idx, BufferPoint& cursor) {
    (void)idx;
    (void)cursor;
  }

  /** @brief Drop all history entries after @p idx. */
  virtual void truncateHistoryAfter(std::size_t) {}
  /** @} */

  /**
   * @name Rendering
   */
  /** @{ */

  /**
   * @brief Compute the 0-indexed screen row of @p point relative to
   *        @p viewFrom for a column width of @p cols.
   *
   * Accounts for line wrapping when @p wrap is true.
   */
  virtual std::size_t screenRow(ColIdx cols, BufferPoint point, ViewFrom viewFrom,
                                bool wrap = true) const;

  /**
   * @brief Append a @p rows × @p cols screen image (with ANSI escapes) to
   *        @p out using the view state in @p params.
   * @return The screen {row, col} of the cursor for positioning.
   */
  virtual std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                            RenderParams const& params) const;

  /**
   * @brief Cached syntax tokens for line @p lineIdx, extending the cache
   *        if necessary.
   *
   * The backing store for findMatchingBracket and similar tools, so
   * repeated lookups do not re-tokenise. Only valid when the buffer's
   * mode uses the C/C++ tokenizer; callers must not request tokens for
   * modes without a cache.
   */
  std::span<SyntaxToken const> cachedLineTokens(std::size_t lineIdx) const;
  /** @} */

  /**
   * @brief Minimal cursor/viewport state stashed on a buffer when a panel
   *        switches away, so a new BufferView can restore it later.
   */
  struct SavedViewState {
    BufferPoint point;
    ViewFrom viewFrom;
    std::optional<BufferPoint> mark;
    std::optional<std::size_t> desiredCol;
    bool wrapMode = false;
    bool showWhitespace = false;
    bool showLineNumbers = false;
    bool highlightCurrentLine = false;
  };
  /** @brief Stash saved view state on this buffer. */
  void setSavedView(SavedViewState s) { _savedView = s; }
  /** @brief Retrieve any stashed view state. */
  std::optional<SavedViewState> savedView() const { return _savedView; }
  /** @brief Discard any stashed view state. */
  void clearSavedView() { _savedView.reset(); }

protected:
  explicit Buffer(std::string name);

  std::string _name;
  BufferMode _mode = BufferMode::Txt;
  /**
   * @brief Line storage; always holds at least one (possibly empty) line.
   *
   * The mutation helpers below maintain that invariant.
   */
  std::vector<LinePtr> _lines{std::make_shared<std::string const>()};

  /**
   * @brief Detach line @p idx from other holders and return a mutable
   *        reference to its string.
   *
   * If the LinePtr is uniquely held the string is mutated in place via
   * const_cast; otherwise a fresh copy is allocated and stored back.
   */
  std::string& mutLine(std::size_t idx) {
    auto& p = _lines[idx];
    if (p.use_count() > 1)
      p = std::make_shared<std::string const>(*p);
    return const_cast<std::string&>(*p);
  }

  /** @brief Construct a LinePtr from an rvalue/lvalue string. */
  static LinePtr makeLine(std::string s) {
    return std::make_shared<std::string const>(std::move(s));
  }
  /** @brief Construct a LinePtr from a string view. */
  static LinePtr makeLine(std::string_view sv) { return std::make_shared<std::string const>(sv); }
  /** @brief Construct an empty LinePtr. */
  static LinePtr makeLine() { return std::make_shared<std::string const>(); }
  bool _modified = false;
  bool _hasConflicts = false;
  std::size_t _docVersion = 0; /**< Incremented on every edit. */
  std::optional<SavedViewState> _savedView;

  /** @brief Cache of the most recent findMatchingBracket() result. */
  struct BracketMatchCache {
    BufferPoint point;
    std::size_t docVersion = SIZE_MAX;
    std::optional<BufferPoint> result;
  };
  mutable BracketMatchCache _bracketCache;

  /**
   * @brief Per-line syntax tokenisation cache entry.
   *
   * Each entry stores the tokenisation result for a line and the
   * multi-line state at the *end* of that line (the state that feeds the
   * next line). Lines [0, _synCacheValidUpto) have valid entries.
   */
  struct LineSyntaxCache {
    SyntaxState stateAfter;          /**< State at end of this line. */
    std::vector<SyntaxToken> tokens; /**< Per-byte token classification. */
  };
  mutable std::vector<LineSyntaxCache> _synCache;
  mutable std::size_t _synCacheValidUpto = 0;

  /** @brief Scan lines for git merge-conflict markers and set _hasConflicts. */
  void scanForConflicts();
  /** @brief Mark all cache entries from @p fromLine onwards as stale. */
  void invalidateSynCache(std::size_t fromLine);
  /** @brief Ensure the syntax cache covers lines [0, upToLine). Called by render(). */
  void ensureSynCache(std::size_t upToLine) const;
};
