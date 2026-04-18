#include "version_history.hpp"

void VersionHistory::capture(std::vector<std::string> const& currentLines, BufferPoint cursor) {
  VersionSnapshot snap;
  snap.timestamp = std::chrono::system_clock::now();
  snap.cursor = cursor;
  snap.lines.reserve(currentLines.size());

  if (_versions.empty()) {
    for (auto const& s : currentLines)
      snap.lines.push_back(std::make_shared<std::string const>(s));
  } else {
    auto const& prev = _versions.back().lines;
    for (std::size_t i = 0; i < currentLines.size(); ++i) {
      if (i < prev.size() && *prev[i] == currentLines[i])
        snap.lines.push_back(prev[i]);
      else
        snap.lines.push_back(std::make_shared<std::string const>(currentLines[i]));
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
  // Drop the oldest kDropOldest entries.
  _versions.erase(_versions.begin(), _versions.begin() + kDropOldest);
  // Thin odd middle indices kThinFrom, kThinFrom+2, ..., kThinTo (back-to-front).
  for (std::size_t i = kThinTo; i >= kThinFrom; i -= 2) {
    if (i < _versions.size())
      _versions.erase(_versions.begin() + i);
    if (i < 2)
      break;
  }
}
