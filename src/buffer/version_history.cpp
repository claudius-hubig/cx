#include "version_history.hpp"

#include <algorithm>

void VersionHistory::capture(LineStore const& currentLines, BufferPoint cursor) {
  _versions.push_back({currentLines, std::chrono::system_clock::now(), cursor});
  pruneIfNeeded();
}

void VersionHistory::truncateAfter(std::size_t idx) {
  if (idx + 1 < _versions.size())
    _versions.erase(_versions.begin() + static_cast<std::ptrdiff_t>(idx + 1), _versions.end());
}

/**
 * @brief Drop every other entry (1, 3, 5, …) among the oldest kThinCount
 *        once the history exceeds kMaxVersions.
 *
 * Every snapshot is complete, so dropping one needs no fix-up of its
 * neighbours; releasing it costs one reference-count decrement per chunk
 * plus freeing the chunks and lines only it held.
 */
void VersionHistory::pruneIfNeeded() {
  if (_versions.size() <= kMaxVersions)
    return;
  std::size_t const thin = std::min(kThinCount, _versions.size());
  std::size_t kept = 0;
  for (std::size_t i = 0; i < _versions.size(); ++i)
    if (i >= thin || i % 2 == 0) {
      if (kept != i)
        _versions[kept] = std::move(_versions[i]);
      ++kept;
    }
  _versions.erase(_versions.begin() + static_cast<std::ptrdiff_t>(kept), _versions.end());
}
