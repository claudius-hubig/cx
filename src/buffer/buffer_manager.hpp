#pragma once

#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/text_buffer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BufferSink;

class BufferManager {
public:
  /// Construct the manager. Creates a default "*1*" buffer and calls
  /// sink.setBuffer() with it.
  explicit BufferManager(BufferSink& sink);

  /// Open (or return existing) buffer for the given path. Canonicalizes
  /// via weakly_canonical(absolute()). Returns existing buffer if already
  /// open; otherwise reads file into a new TextBuffer. Calls touch().
  Buffer& open(std::filesystem::path const& path);

  Buffer& helpBuffer() { return *_helpBuffer; }

  /// Return the buffer list buffer, creating it lazily on first call.
  /// Always refreshes the display with current buffer names.
  BufferListBuffer& bufferListBuffer();

  /// Find a buffer by exact name. Returns nullptr if not found.
  Buffer* find(std::string const& name);

  /// Create a new empty buffer with the given name. Idempotent: returns
  /// the existing buffer if one with the same name exists. Calls touch().
  Buffer& create(std::string name);

  /// Return sorted names of all buffers (excluding help and buffer list).
  std::vector<std::string> bufferNames() const;

  /// Return a short display name: just the filename if unambiguous among
  /// open buffers, or the full path if another buffer shares the same
  /// filename. Non-path names (like "*1*") are returned as-is.
  /// Uses a cached map that is refreshed when buffers are opened/created.
  std::string displayName(std::string const& name) const;

  /// Return completions for the given prefix, matching buffer display
  /// names and filesystem entries. Each pair: {displayName, bufferName}.
  /// Results are sorted and deduplicated.
  std::vector<std::pair<std::string, std::string>> completions(std::string const& prefix) const;

  /// Change a buffer's path and re-key it in the map. Silently returns
  /// if oldName is not found. Updates MRU to reflect the new name.
  void rename(std::string const& oldName, std::filesystem::path newPath);

  /// Remove the named buffer. Always leaves >= 1 buffer (creates "*1*"
  /// if all killed). Returns the next buffer (most recently used).
  /// Takes name by value to prevent use-after-free when the caller
  /// passes a reference to a string owned by the buffer being killed.
  Buffer& kill(std::string name);

  /// Mark a buffer as most recently used (moves to front of MRU list).
  void touch(std::string const& name);

  /// Return the name of the second most recently used buffer, or empty
  /// string if fewer than 2 entries.
  std::string secondMRU() const;

private:
  void refreshDisplayNames();

  std::unique_ptr<HelpBuffer> _helpBuffer;
  std::unique_ptr<BufferListBuffer> _bufferListBuffer;
  std::unordered_map<std::string, std::unique_ptr<TextBuffer>> _buffers;
  std::unordered_map<std::string, std::string> _displayNames;
  std::vector<std::string> _mru; // most recently used first
  BufferSink& _sink;

  // Cache for directory listings used by completions().
  struct DirCacheEntry {
    std::string dir;
    std::vector<std::pair<std::string, bool>> entries; // {filename, isDir}
  };
  mutable DirCacheEntry _dirCache;
};
