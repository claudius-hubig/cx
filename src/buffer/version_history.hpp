#pragma once

/**
 * @file version_history.hpp
 * @brief VersionHistory — an append-only history of buffer snapshots with
 *        delta compression, backing undo and version browsing.
 */

#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief One captured version of a buffer.
 *
 * A snapshot either stores the full line vector (@c lines) or, for cheap
 * single-line edits, only the lines that changed since its predecessor
 * (@c patches). Delta snapshots are materialised lazily on access.
 */
struct VersionSnapshot {
  /** @brief Alias retained for callers; the canonical type lives in types.hpp. */
  using LinePtr = ::LinePtr;
  mutable std::vector<LinePtr> lines;          /**< Full line storage (empty for a delta). */
  std::chrono::system_clock::time_point timestamp; /**< When the snapshot was captured. */
  BufferPoint cursor;                          /**< Cursor position at capture time. */

  /** @brief A single changed line in a delta snapshot. */
  struct LinePatch {
    std::size_t line;
    LinePtr content;
  };
  /**
   * @brief Changed lines relative to the previous version.
   *
   * When @c lines is empty and @c patches is non-empty, this snapshot is
   * a delta materialised lazily on access.
   */
  std::vector<LinePatch> patches;

  /** @brief True if this snapshot is stored as a delta from its predecessor. */
  bool isDelta() const { return lines.empty() && !patches.empty(); }
};

/**
 * @brief Append-only history of buffer snapshots with O(1) delta capture.
 *
 * Single-line edits (lineShift == 0) are stored as lightweight deltas that
 * record only the changed line, making capture O(1) rather than O(N).
 * Deltas materialise lazily on at(). During pruning, dropped entries'
 * patches are merged into their successors instead of being materialised,
 * keeping prune cost proportional to the number of dropped full snapshots
 * rather than the total history size.
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
   * Bounds steady-state memory at O(kMaxVersions * average
   * lines-changed-per-edit) and the worst-case capture pause (a prune is
   * amortised over kMaxVersions - kThinCount/2 captures).
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
   * @brief Max consecutive deltas before capture forces a full snapshot.
   *
   * Bounds the worst-case at() walk back to a base snapshot at
   * O(kMaxDeltaChain) line-pointer copies.
   */
  static constexpr std::size_t kMaxDeltaChain = 32;

  /**
   * @brief Capture a snapshot from the live buffer's line vector.
   *
   * The LinePtrs are reference-counted, so this is O(N pointers) on both
   * the delta path (single-line replacement) and the full path — never
   * O(N strings).
   *
   * @param currentLines The buffer's current line vector.
   * @param cursor Cursor position to record with the snapshot.
   * @param dirtyFrom First line changed since the last capture (SIZE_MAX
   *        means "unknown"; forces a full snapshot).
   * @param lineShift Net change in line count from the edit; non-zero
   *        forces a full snapshot.
   */
  void capture(std::vector<LinePtr> const& currentLines, BufferPoint cursor,
               std::size_t dirtyFrom = SIZE_MAX, int lineShift = 0);

  /**
   * @brief Capture the initial snapshot from a freshly loaded line vector.
   *
   * Same semantics as capture() but skips the prior-snapshot equality scan
   * (there is no prior snapshot to compare against).
   */
  void captureInitial(std::vector<LinePtr> const& lines, BufferPoint cursor);

  /** @brief Number of snapshots currently retained. */
  std::size_t size() const { return _versions.size(); }
  /** @brief True if no snapshots are retained. */
  bool empty() const { return _versions.empty(); }
  /**
   * @brief Access the snapshot at @p idx, materialising it if it is a delta.
   * @throws std::out_of_range if @p idx is out of bounds.
   */
  VersionSnapshot const& at(std::size_t idx) const {
    materialize(idx);
    return _versions.at(idx);
  }

  /**
   * @brief Drop every snapshot after @p idx.
   *
   * Used when committing a history-mode rewind.
   */
  void truncateAfter(std::size_t idx);

  /** @brief Discard all snapshots and reset internal state. */
  void clear() {
    _versions.clear();
    _pendingDestroy.clear();
    _deltaChainLen = 0;
  }

private:
  /** @brief Resolve a delta at @p idx into a full snapshot in place. */
  void materialize(std::size_t idx) const;
  /** @brief Prune the oldest entries when the history exceeds kMaxVersions. */
  void pruneIfNeeded();

  std::deque<VersionSnapshot> _versions;
  std::deque<VersionSnapshot> _pendingDestroy;
  std::size_t _deltaChainLen = 0;
};
