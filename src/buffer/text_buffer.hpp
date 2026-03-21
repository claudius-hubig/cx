#pragma once

#include "buffer.hpp"

#include <filesystem>
#include <optional>
#include <string>

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
  std::expected<void, std::string> revert();

private:
  std::optional<std::filesystem::path> _path;
};
