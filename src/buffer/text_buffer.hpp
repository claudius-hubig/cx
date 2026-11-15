#pragma once

/**
 * @file text_buffer.hpp
 * @brief TextBuffer — the concrete, editable buffer backed by a file on
 *        disk, with version history, line-ending detection, and
 *        on-disk-change tracking.
 */

#include "buffer.hpp"
#include "version_history.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

/**
 * @brief Line-ending style detected when reading a file.
 *
 * `\r` bytes are left intact in the line strings at load time; callers can
 * inspect this value and call normalizeLineEndings() (e.g. after user
 * consent) to translate non-Unix endings to `\n`.
 */
enum class LineEndingStyle {
  Unix,  /**< Only `\n` (or no line endings). */
  Crlf,  /**< Only `\r\n`. */
  Cr,    /**< Only lone `\r`. */
  Mixed, /**< More than one of the above. */
};

/** @brief Editable, file-backed buffer. */
class TextBuffer : public Buffer {
public:
  /**
   * @brief Construct a buffer with @p name and no backing file.
   *
   * The buffer starts empty and unmodified.
   */
  explicit TextBuffer(std::string name);

  /**
   * @brief Construct a buffer that reads its initial content from @p path.
   *
   * If the file is missing or unreadable the buffer is empty but
   * hasPath() still returns true so save() can write to it.
   */
  TextBuffer(std::string name, std::filesystem::path const& path);

  /** @brief True while a view is browsing history (makes the buffer immutable). */
  bool readonly() const override { return _browsingHistory; }

  bool hasPath() const override { return _path.has_value(); }

  /**
   * @brief The version history.
   *
   * Snapshots are appended on every edit by recordVersion() and survive
   * both save() and revert(). See version_history.hpp for the
   * capacity/pruning policy.
   */
  VersionHistory const& history() const { return _history; }

  bool browsingHistory() const override { return _browsingHistory; }
  void setBrowsingHistory(bool b) override {
    if (b && !_browsingHistory)
      _modifiedBeforeBrowsing = _modified;
    _browsingHistory = b;
  }

  /**
   * @brief Append a snapshot to the history.
   *
   * Called by BufferView after each successful edit. Silently no-ops
   * while browsingHistory() is true, so any helper or script-driven edit
   * that bypasses BufferView cannot corrupt the history chain by
   * inserting a new version mid-browse.
   */
  void recordVersion(BufferPoint cursor) override {
    if (_browsingHistory)
      return;
    _history.capture(_lines, cursor);
  }

  /**
   * @brief Replace buffer contents with @p lines.
   *
   * Bumps docVersion and invalidates the syntax cache; does NOT record a
   * new version.
   */
  void applySnapshot(LineStore lines);

  VersionHistoryInfo versionHistoryInfo() const override {
    return {_history.size(), _history.empty()};
  }

  void applyVersionAt(std::size_t idx, BufferPoint& cursor) override {
    applySnapshot(_history.linesAt(idx));
    cursor = _history.cursorAt(idx);
    // The newest version is the text the buffer held when browsing began
    // (it is read-only meanwhile), so showing it again is not a change.
    if (_browsingHistory && idx + 1 == _history.size())
      _modified = _modifiedBeforeBrowsing;
  }

  void truncateHistoryAfter(std::size_t idx) override { _history.truncateAfter(idx); }

  /**
   * @brief Set the backing file path and update @c _name to the path
   *        string.
   *
   * Must coordinate with BufferManager::rename() to keep the buffer map
   * and MRU list in sync.
   */
  void setPath(std::filesystem::path p);

  /**
   * @brief Write buffer lines to the backing file as a truncating binary
   *        write.
   *
   * Clears the modified flag on success.
   * @return An empty expected on success, or an error string on failure.
   */
  std::expected<void, std::string> save() override;

  /**
   * @brief Reload buffer contents from the backing file.
   *
   * Bumps docVersion but does not touch view state. Refused while a
   * view is browsing the history (browsingHistory()), which shows and
   * may commit an older version.
   * @return An error if refused or the file cannot be read.
   */
  std::expected<void, std::string> revert() override;

  /**
   * @brief Line-ending style detected when the file was last read.
   *
   * Returns @c Unix for pathless buffers, empty files, and `\n`-only
   * files. The flag is cleared by acknowledgeLineEndingStyle() so a
   * buffer is flagged only once per load.
   */
  LineEndingStyle lineEndingStyle() const { return _lineEndingStyle; }

  /**
   * @brief Clear the detected style back to @c Unix.
   *
   * Call after the user has been notified or after normalizeLineEndings()
   * so the same buffer is not prompted for again on subsequent view
   * switches.
   */
  void acknowledgeLineEndingStyle() { _lineEndingStyle = LineEndingStyle::Unix; }

  /**
   * @brief Translate CRLF or lone-CR line endings to `\n`.
   *
   * For CRLF, strips trailing `\r` from every line; for CR, splits each
   * line on `\r` into new lines. Marks the buffer modified, bumps
   * docVersion, and invalidates the syntax cache. A no-op for @c Unix and
   * @c Mixed styles (Mixed cannot be unambiguously translated).
   */
  void normalizeLineEndings();

  /**
   * @brief Whether the backing file's mtime or size differs from the
   *        baseline captured at the last load or save.
   *
   * Returns false for pathless buffers, files that no longer exist, and
   * on any stat error — the conservative no-warning answer, since the
   * caller handles the next save() outright.
   */
  bool changedOnDisk() const;

private:
  /** @brief Set the BufferMode from the file extension or `#!` line. */
  void detectMode();
  /** @brief Read the backing file into @c _lines, populating @c _lineEndingStyle. */
  void loadFromFile(std::filesystem::path const& path);

  /**
   * @brief Capture the on-disk mtime and size of @c _path as the baseline
   *        for changedOnDisk().
   *
   * Called at the end of loadFromFile() and after a successful save().
   * Leaves the fields untouched on stat failure: callers treat that as
   * "no known baseline" and skip the warning.
   */
  void captureDiskStamp();

  std::optional<std::filesystem::path> _path;
  LineEndingStyle _lineEndingStyle = LineEndingStyle::Unix;
  VersionHistory _history;
  bool _browsingHistory = false;
  /** @brief modified() when history browsing began; see applyVersionAt(). */
  bool _modifiedBeforeBrowsing = false;
  /**
   * @brief mtime of the backing file at the last load/save; nullopt while
   *        no baseline is known.
   */
  std::optional<std::filesystem::file_time_type> _diskMtime;
  /** @brief Size of the backing file at the last load/save; nullopt if unknown. */
  std::optional<std::uintmax_t> _diskSize;
};
