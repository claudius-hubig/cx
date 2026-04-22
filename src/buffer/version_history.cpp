#include "version_history.hpp"

#include <algorithm>

void VersionHistory::capture(std::vector<std::string> const& currentLines, BufferPoint cursor,
                             std::size_t dirtyFrom, int lineShift) {
  if (!_pendingDestroy.empty())
    _pendingDestroy.pop_front();

  VersionSnapshot snap;
  snap.timestamp = std::chrono::system_clock::now();
  snap.cursor = cursor;

  if (_versions.empty()) {
    snap.lines.reserve(currentLines.size());
    for (auto const& s : currentLines)
      snap.lines.push_back(std::make_shared<std::string const>(s));
    _deltaChainLen = 0;
  } else if (dirtyFrom != SIZE_MAX && lineShift == 0 && _deltaChainLen < kMaxDeltaChain) {
    snap.patches.push_back(
        {dirtyFrom, std::make_shared<std::string const>(currentLines[dirtyFrom])});
    ++_deltaChainLen;
  } else {
    materialize(_versions.size() - 1);
    auto const& prev = _versions.back().lines;
    auto prevSize = prev.size();

    snap.lines.reserve(currentLines.size());
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
    _deltaChainLen = 0;
  }

  _versions.push_back(std::move(snap));
  pruneIfNeeded();
}

void VersionHistory::materialize(std::size_t idx) const {
  if (idx >= _versions.size() || !_versions[idx].isDelta())
    return;

  std::size_t base = idx;
  while (base > 0 && _versions[base].isDelta())
    --base;

  auto lines = _versions[base].lines;
  for (std::size_t i = base + 1; i <= idx; ++i)
    for (auto const& p : _versions[i].patches)
      lines[p.line] = p.content;
  _versions[idx].lines = std::move(lines);
}

void VersionHistory::truncateAfter(std::size_t idx) {
  if (idx + 1 < _versions.size())
    _versions.erase(_versions.begin() + idx + 1, _versions.end());
  _deltaChainLen = 0;
  for (auto it = _versions.rbegin(); it != _versions.rend() && it->isDelta(); ++it)
    ++_deltaChainLen;
}

void VersionHistory::pruneIfNeeded() {
  if (_versions.size() <= kMaxVersions)
    return;

  std::size_t thin = std::min(kThinCount, _versions.size());

  // Merge dropped entries' patches into their successors so delta chains
  // stay valid after the drop. Only full-snapshot drops require an O(N)
  // materialization of the successor; delta-delta merges are O(patches).
  for (std::size_t i = 1; i < thin && i + 1 < _versions.size(); i += 2) {
    auto& dropped = _versions[i];
    auto& kept = _versions[i + 1];

    if (dropped.isDelta() && kept.isDelta()) {
      kept.patches.insert(kept.patches.begin(), dropped.patches.begin(), dropped.patches.end());
    } else if (!dropped.isDelta() && kept.isDelta()) {
      kept.lines = dropped.lines;
      for (auto const& p : kept.patches)
        kept.lines[p.line] = p.content;
      kept.patches.clear();
    }
    _pendingDestroy.push_back(std::move(dropped));
  }

  std::deque<VersionSnapshot> kept;
  for (std::size_t i = 0; i < thin; i += 2)
    kept.push_back(std::move(_versions[i]));
  for (std::size_t i = thin; i < _versions.size(); ++i)
    kept.push_back(std::move(_versions[i]));
  _versions = std::move(kept);
  _deltaChainLen = 0;
  for (auto it = _versions.rbegin(); it != _versions.rend() && it->isDelta(); ++it)
    ++_deltaChainLen;
}
