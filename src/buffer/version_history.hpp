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
/// Capped at 100 entries; when exceeded, the oldest 10 are dropped and
/// odd middle positions 31, 33, ..., 89 are thinned, leaving 61 entries.
class VersionHistory {
public:
  static constexpr std::size_t kMaxVersions = 100;
  static constexpr std::size_t kDropOldest = 10;
  static constexpr std::size_t kThinFrom = 31;
  static constexpr std::size_t kThinTo = 89;

  /// Append a new snapshot for `currentLines` at `cursor`. Reuses LinePtrs
  /// from the previous snapshot where line content matches byte-for-byte.
  void capture(std::vector<std::string> const& currentLines, BufferPoint cursor);

  std::size_t size() const { return _versions.size(); }
  bool empty() const { return _versions.empty(); }
  VersionSnapshot const& at(std::size_t idx) const { return _versions[idx]; }

  /// Drop all versions with index > idx.
  void truncateAfter(std::size_t idx);

  void clear() { _versions.clear(); }

private:
  void pruneIfNeeded();

  std::deque<VersionSnapshot> _versions;
};
