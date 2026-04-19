#include "version_history.hpp"

#include <algorithm>

void VersionHistory::capture(std::vector<std::string> const& currentLines, BufferPoint cursor,
                             std::size_t dirtyFrom, int lineShift) {
  if (!_pendingDestroy.empty())
    _pendingDestroy.pop_front();

  VersionSnapshot snap;
  snap.timestamp = std::chrono::system_clock::now();
  snap.cursor = cursor;
  snap.lines.reserve(currentLines.size());

  if (_versions.empty()) {
    for (auto const& s : currentLines)
      snap.lines.push_back(std::make_shared<std::string const>(s));
  } else {
    auto const& prev = _versions.back().lines;
    auto prevSize = prev.size();

    if (dirtyFrom == SIZE_MAX) {
      for (std::size_t i = 0; i < currentLines.size(); ++i) {
        if (i < prevSize && *prev[i] == currentLines[i])
          snap.lines.push_back(prev[i]);
        else
          snap.lines.push_back(std::make_shared<std::string const>(currentLines[i]));
      }
    } else {
      std::size_t safePrefix = std::min({dirtyFrom, currentLines.size(), prevSize});
      for (std::size_t i = 0; i < safePrefix; ++i)
        snap.lines.push_back(prev[i]);

      std::size_t modCount = static_cast<std::size_t>(std::max(1, 1 + lineShift));
      std::size_t afterNew = dirtyFrom + modCount;
      for (std::size_t i = dirtyFrom; i < afterNew && i < currentLines.size(); ++i)
        snap.lines.push_back(std::make_shared<std::string const>(currentLines[i]));

      for (std::size_t i = afterNew; i < currentLines.size(); ++i) {
        auto oldIdx = static_cast<std::size_t>(static_cast<long long>(i) - lineShift);
        if (oldIdx < prevSize)
          snap.lines.push_back(prev[oldIdx]);
        else
          snap.lines.push_back(std::make_shared<std::string const>(currentLines[i]));
      }
    }
  }

  _versions.push_back(std::move(snap));
  pruneIfNeeded();
}

void VersionHistory::truncateAfter(std::size_t idx) {
  if (idx + 1 < _versions.size())
    _versions.erase(_versions.begin() + idx + 1, _versions.end());
}

void VersionHistory::pruneIfNeeded() {
  if (_versions.size() <= kMaxVersions)
    return;
  std::size_t thin = std::min(kThinCount, _versions.size());
  for (std::size_t i = 1; i < thin; i += 2)
    _pendingDestroy.push_back(std::move(_versions[i]));
  std::deque<VersionSnapshot> kept;
  for (std::size_t i = 0; i < thin; i += 2)
    kept.push_back(std::move(_versions[i]));
  for (std::size_t i = thin; i < _versions.size(); ++i)
    kept.push_back(std::move(_versions[i]));
  _versions = std::move(kept);
}
