#pragma once

/**
 * @file version_history.hpp
 * @brief VersionHistory — an append-only history of whole-buffer
 *        snapshots, backing undo and version browsing.
 */

#include "line_store.hpp"
#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <deque>

/** @brief One captured version of a buffer. */
struct VersionSnapshot {
  /** @brief The buffer's lines; a LineStore copy, sharing chunks and lines with the buffer. */
  LineStore lines;
  std::chrono::system_clock::time_point timestamp; /**< When the snapshot was captured. */
  BufferPoint cursor;                              /**< Cursor position at capture time. */
};

/**
 * @brief Append-only history of whole-buffer snapshots.
 *
 * Every version is a complete copy of the buffer's LineStore. A copy
 * shares the store's chunks, so capturing costs one pointer per chunk
 * (O(N / chunk size)) whatever the edit was, and versions keep only the
 * chunks and lines that differ between them. Reading a version is a copy
 * of the same kind and never changes the history.
 *
 * Pruning thins every other entry in the oldest @c kThinCount slots, so
 * when @c _versions.size() reaches @c kMaxVersions + 1 the post-prune size
 * is `(kThinCount / 2) + (size - kThinCount)` (56 for the default
 * constants), not @c kThinCount. The working size therefore oscillates
 * roughly in [(kThinCount/2 + 11), kMaxVersions+1].
 */
class VersionHistory {
public:
  /**
   * @brief Cap on retained snapshots.
   *
   * Bounds steady-state memory at O(kMaxVersions × the chunks and lines
   * changed per edit) plus one chunk index per version.
   */
  static constexpr std::size_t kMaxVersions = 100;
  /**
   * @brief Number of oldest entries inspected when pruning.
   *
   * Smaller values prune more aggressively (cheaper, less undo depth);
   * larger values keep more recent history at the cost of more frequent
   * prunes.
   */
  static constexpr std::size_t kThinCount = 90;

  /**
   * @brief Append a snapshot of @p currentLines with @p cursor.
   *
   * O(N / chunk size) pointer copies; may prune (see the class notes).
   */
  void capture(LineStore const& currentLines, BufferPoint cursor);

  /** @brief Number of snapshots currently retained. */
  std::size_t size() const { return _versions.size(); }
  /** @brief True if no snapshots are retained. */
  bool empty() const { return _versions.empty(); }
  /**
   * @brief The lines of version @p idx: a copy of its store, O(N / chunk
   *        size), sharing its chunks.
   * @throws std::out_of_range if @p idx is out of bounds.
   */
  LineStore linesAt(std::size_t idx) const { return _versions.at(idx).lines; }
  /**
   * @brief When version @p idx was captured. O(1).
   * @throws std::out_of_range if @p idx is out of bounds.
   */
  std::chrono::system_clock::time_point timestampAt(std::size_t idx) const {
    return _versions.at(idx).timestamp;
  }
  /**
   * @brief Cursor position recorded with version @p idx. O(1).
   * @throws std::out_of_range if @p idx is out of bounds.
   */
  BufferPoint cursorAt(std::size_t idx) const { return _versions.at(idx).cursor; }

  /**
   * @brief Drop every snapshot after @p idx.
   *
   * Used when committing a history-mode rewind.
   */
  void truncateAfter(std::size_t idx);

  /** @brief Discard all snapshots. */
  void clear() { _versions.clear(); }

private:
  /** @brief Prune the oldest entries when the history exceeds kMaxVersions. */
  void pruneIfNeeded();

  std::deque<VersionSnapshot> _versions;
};
