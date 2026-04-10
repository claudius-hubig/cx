#pragma once

#include "buffer.hpp"

#include <filesystem>
#include <optional>
#include <string>

/// Line ending style detected when reading a file. `\r` bytes are left
/// intact in the line strings at load time — callers can inspect this
/// value and call `normalizeLineEndings()` (e.g. after user consent)
/// to translate non-Unix endings to `\n`.
enum class LineEndingStyle {
  Unix,  ///< Only `\n` (or no line endings).
  Crlf,  ///< Only `\r\n`.
  Cr,    ///< Only lone `\r`.
  Mixed, ///< More than one of the above.
};

class TextBuffer : public Buffer {
public:
  /// Construct a buffer with the given name and no backing file.
  /// The buffer starts empty and unmodified.
  explicit TextBuffer(std::string name);

  /// Construct a buffer that reads its initial content from path.
  /// If the file is missing or unreadable the buffer is empty but
  /// hasPath() still returns true so save() can write to it.
  TextBuffer(std::string name, std::filesystem::path const& path);

  bool readonly() const override { return false; }

  bool hasPath() const override { return _path.has_value(); }

  /// Set the backing file path. Updates _name to the path string.
  /// Must coordinate with BufferManager::rename() to keep the map
  /// and MRU in sync.
  void setPath(std::filesystem::path p);

  /// Write buffer lines to _path as a truncating binary write. Returns an
  /// empty expected on success, or an error string on failure.
  /// Clears _modified on success.
  std::expected<void, std::string> save() override;

  /// Reload buffer contents from the backing file. Resets point and
  /// viewport to the start. Returns an error if the file cannot be read.
  std::expected<void, std::string> revert() override;

  /// Line ending style detected when the file was last read. Returns
  /// `Unix` for pathless buffers, empty files, and files with only `\n`
  /// line endings. The flag is cleared by `acknowledgeLineEndingStyle()`
  /// so a buffer is only flagged once per load.
  LineEndingStyle lineEndingStyle() const { return _lineEndingStyle; }

  /// Clear the detected style back to `Unix`. Call after the user has
  /// been notified or after calling `normalizeLineEndings()` so that
  /// the same buffer is not prompted for again on subsequent view
  /// switches.
  void acknowledgeLineEndingStyle() { _lineEndingStyle = LineEndingStyle::Unix; }

  /// Translate CRLF or lone-CR line endings to `\n`. For CRLF, strips
  /// trailing `\r` from every line. For CR, splits each line on `\r`
  /// to form new lines. Marks the buffer modified, bumps docVersion,
  /// and invalidates the syntax cache. No-op for Unix and Mixed styles
  /// (Mixed cannot be unambiguously translated).
  void normalizeLineEndings();

private:
  void detectMode();
  /// Read the backing file into `_lines`, populating `_lineEndingStyle`.
  /// Leaves `\r` bytes intact in the line content.
  void loadFromFile(std::filesystem::path const& path);

  std::optional<std::filesystem::path> _path;
  LineEndingStyle _lineEndingStyle = LineEndingStyle::Unix;
};
