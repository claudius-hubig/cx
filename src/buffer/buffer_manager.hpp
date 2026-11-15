#pragma once

/**
 * @file buffer_manager.hpp
 * @brief BufferManager — owns every buffer's lifecycle, the MRU ordering,
 *        display-name disambiguation, and switch-buffer completions.
 */

#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/dir_buffer.hpp"
#include "buffer/grep_buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/paste_ring_buffer.hpp"
#include "buffer/text_buffer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BufferSink;

/**
 * @brief Owns all buffers and mediates switching between them.
 *
 * Holds every TextBuffer (keyed by canonical path or synthetic name) and
 * DirBuffer (keyed by canonical directory), the singleton HelpBuffer, and
 * lazily-created BufferListBuffer and PasteRingBuffer. Pushes buffer
 * switches to the display layer through a BufferSink, so it never depends
 * on Screen concretely.
 */
class BufferManager {
public:
  /**
   * @brief Construct the manager.
   *
   * Creates a default "*1*" buffer and calls @c sink.setBuffer() with it.
   */
  explicit BufferManager(BufferSink& sink);

  /**
   * @brief Open (or return the existing) buffer for @p path.
   *
   * Canonicalises via weakly_canonical(absolute()); reads the file into a
   * new TextBuffer when not already open. Calls touch().
   */
  Buffer& open(std::filesystem::path const& path);

  /** @brief The singleton help buffer. */
  Buffer& helpBuffer() { return *_helpBuffer; }

  /**
   * @brief The buffer-list buffer, created lazily and always refreshed
   *        with the current buffer names.
   */
  BufferListBuffer& bufferListBuffer();

  /**
   * @brief The paste-ring buffer, created lazily and always refreshed with
   *        the current paste-ring contents.
   */
  PasteRingBuffer& pasteRingBuffer(PasteRing const& pr);

  /**
   * @brief The find-in-files buffer, created lazily and always refreshed
   *        with the results of the search just run (see GrepBuffer::refresh).
   */
  GrepBuffer& grepBuffer(std::string pattern, std::filesystem::path root, std::vector<GrepHit> hits,
                         bool truncated);

  /**
   * @brief Open (or return the existing) directory buffer for @p dir.
   *
   * Canonicalises the path; creates a new DirBuffer when not already
   * open, and re-reads the listing of one that is (the directory may have
   * changed on disk), keeping its saved cursor on the same entry. Calls
   * touch().
   */
  DirBuffer& openDir(std::filesystem::path const& dir);

  /** @brief Find a buffer by exact name, or nullptr if not found. */
  Buffer* find(std::string const& name);

  /**
   * @brief Create a new empty buffer named @p name.
   *
   * Idempotent: returns the existing buffer if one with that name exists.
   * Calls touch().
   */
  Buffer& create(std::string name);

  /** @brief Sorted names of all buffers (excluding help and buffer list). */
  std::vector<std::string> bufferNames() const;

  /**
   * @brief Short, unambiguous display name for @p name.
   *
   * Just the filename when unambiguous among open buffers, otherwise the
   * full path; non-path names (like "*1*") are returned as-is. Uses a
   * cache refreshed when buffers are opened/created.
   */
  std::string displayName(std::string const& name) const;

  /**
   * @brief Completions for @p prefix, matching buffer display names and
   *        filesystem entries.
   * @return Sorted, deduplicated {displayName, bufferName} pairs.
   */
  std::vector<std::pair<std::string, std::string>> completions(std::string const& prefix) const;

  /**
   * @brief Change a buffer's path and re-key it in the map.
   *
   * @p newPath is made absolute and symlink-resolved like open() does, so
   * the new key matches what opening that file would produce. Updates the
   * MRU to reflect the new name.
   * @return false, leaving every buffer untouched, if @p oldName is not
   *         found or another buffer is already registered under the new
   *         path.
   */
  [[nodiscard]] bool rename(std::string const& oldName, std::filesystem::path const& newPath);

  /**
   * @brief Remove the named buffer.
   *
   * Always leaves at least one buffer alive (creates "*1*" if all are
   * killed). Takes @p name by value to avoid a use-after-free when the
   * caller passes a string owned by the buffer being killed.
   * @return The next buffer to display (most recently used).
   */
  Buffer& kill(std::string name);

  /** @brief Mark a buffer most-recently-used (front of the MRU list). */
  void touch(std::string const& name);

  /**
   * @brief Name of the second-most-recently-used buffer.
   * @return The name, or an empty string if fewer than two entries exist.
   */
  std::string secondMRU() const;

private:
  /** @brief Recompute the display-name disambiguation cache. */
  void refreshDisplayNames();

  std::unique_ptr<HelpBuffer> _helpBuffer;
  std::unique_ptr<BufferListBuffer> _bufferListBuffer;
  std::unique_ptr<PasteRingBuffer> _pasteRingBuffer;
  std::unique_ptr<GrepBuffer> _grepBuffer;
  std::unordered_map<std::string, std::unique_ptr<TextBuffer>> _buffers;
  std::unordered_map<std::string, std::unique_ptr<DirBuffer>> _dirBuffers;
  std::unordered_map<std::string, std::string> _displayNames;
  std::vector<std::string> _mru; /**< Most recently used first. */
  BufferSink& _sink;

  /**
   * @brief Cached directory listing for completions().
   *
   * The canonicalised directory path is cached alongside the listing
   * because weakly_canonical() walks every component with stat() —
   * recomputing it per keystroke is the dominant cost on the cache-hit
   * fast path.
   */
  struct DirCacheEntry {
    std::string dir;
    std::filesystem::path canonDir;
    std::vector<std::pair<std::string, bool>> entries; /**< {filename, isDir}. */
  };
  mutable DirCacheEntry _dirCache;
};
