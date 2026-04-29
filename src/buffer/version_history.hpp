#pragma once

#include "types.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

struct VersionSnapshot {
  using LinePtr = std::shared_ptr<std::string const>;
  mutable std::vector<LinePtr> lines;
  std::chrono::system_clock::time_point timestamp;
  BufferPoint cursor;

  struct LinePatch {
    std::size_t line;
    LinePtr content;
  };
  // When lines is empty and patches is non-empty, this snapshot is a
  // delta from the previous version. Materialized lazily on access.
  std::vector<LinePatch> patches;

  bool isDelta() const { return lines.empty() && !patches.empty(); }
};

/// Append-only history of buffer snapshots with O(1) delta capture.
///
/// Single-line edits (lineShift == 0) are stored as lightweight deltas
/// that record only the changed line, making capture O(1) instead of
/// O(N). Deltas are materialized lazily on at() access. During pruning,
/// dropped entries' patches are merged into their successors rather than
/// materializing full snapshots, keeping prune cost proportional to the
/// number of dropped full snapshots (typically ~3) rather than the total
/// history size.
class VersionHistory {
public:
  static constexpr std::size_t kMaxVersions = 100;
  static constexpr std::size_t kThinCount = 90;
  static constexpr std::size_t kMaxDeltaChain = 32;

  void capture(std::vector<std::string> const& currentLines, BufferPoint cursor,
               std::size_t dirtyFrom = SIZE_MAX, int lineShift = 0);

  /// Move `lines` into a shared block and create aliasing shared_ptrs,
  /// avoiding N individual make_shared allocations. Rebuilds `lines`
  /// by copying from the shared block.
  void captureInitial(std::vector<std::string>& lines, BufferPoint cursor);

  std::size_t size() const { return _versions.size(); }
  bool empty() const { return _versions.empty(); }
  VersionSnapshot const& at(std::size_t idx) const {
    materialize(idx);
    return _versions.at(idx);
  }

  void truncateAfter(std::size_t idx);

  void clear() {
    _versions.clear();
    _pendingDestroy.clear();
    _deltaChainLen = 0;
  }

private:
  void materialize(std::size_t idx) const;
  void pruneIfNeeded();

  std::deque<VersionSnapshot> _versions;
  std::deque<VersionSnapshot> _pendingDestroy;
  std::size_t _deltaChainLen = 0;
};
