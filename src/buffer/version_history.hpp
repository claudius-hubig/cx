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
  std::vector<LinePtr> lines;
  std::chrono::system_clock::time_point timestamp;
  BufferPoint cursor;
};

/// Append-only history of buffer snapshots. Each snapshot reuses
/// shared_ptr<string const> entries from the previous snapshot for lines
/// that did not change, so unchanged regions share storage across versions.
/// Capped at 100 entries; when exceeded, the oldest 90 entries are thinned
/// by dropping every second one (keeping the very oldest), leaving ~56.
class VersionHistory {
public:
  static constexpr std::size_t kMaxVersions = 100;
  static constexpr std::size_t kThinCount = 90;

  /// Append a new snapshot for `currentLines` at `cursor`. Reuses LinePtrs
  /// from the previous snapshot where line content matches byte-for-byte.
  /// When `dirtyFrom` is provided (not SIZE_MAX), lines before the dirty
  /// region and lines after it (shifted by `lineShift`) are copied from
  /// the previous snapshot without comparison.
  void capture(std::vector<std::string> const& currentLines, BufferPoint cursor,
               std::size_t dirtyFrom = SIZE_MAX, int lineShift = 0);

  std::size_t size() const { return _versions.size(); }
  bool empty() const { return _versions.empty(); }
  VersionSnapshot const& at(std::size_t idx) const { return _versions.at(idx); }

  /// Drop all versions with index > idx.
  void truncateAfter(std::size_t idx);

  void clear() {
    _versions.clear();
    _pendingDestroy.clear();
  }

private:
  void pruneIfNeeded();

  std::deque<VersionSnapshot> _versions;
  std::deque<VersionSnapshot> _pendingDestroy;
};
