#include "version_history.hpp"

#include <algorithm>

/**
 * @brief Capture a snapshot of the current buffer state.
 *
 * If this is the first snapshot, stores a full copy. Otherwise, if the edit
 * is a single-line change (dirtyFrom specified, no line count change, and
 * delta chain not too long), stores only the changed line as a delta patch.
 * Full snapshots are taken when line count changes or delta chain limit is reached.
 *
 * Automatically prunes old snapshots if the history exceeds kMaxVersions.
 */
void VersionHistory::capture(std::vector<LinePtr> const& currentLines, BufferPoint cursor,
                             std::size_t dirtyFrom, int lineShift) {
  if (!_pendingDestroy.empty())
    _pendingDestroy.pop_front();

  VersionSnapshot snap;
  snap.timestamp = std::chrono::system_clock::now();
  snap.cursor = cursor;

  if (_versions.empty()) {
    // Share LinePtrs directly; no string allocation per line.
    snap.lines = currentLines;
    _deltaChainLen = 0;
  } else if (dirtyFrom != SIZE_MAX && lineShift == 0 && _deltaChainLen < kMaxDeltaChain) {
    // Delta path: just record a single LinePtr for the changed line.
    snap.patches.push_back({dirtyFrom, currentLines[dirtyFrom]});
    ++_deltaChainLen;
  } else {
    // Full snapshot path. Share the LinePtrs from currentLines as-is —
    // unchanged lines naturally retain pointer identity across snapshots
    // because Buffer::mutLine only allocates a fresh string when the
    // existing one is shared.
    snap.lines = currentLines;
    (void)dirtyFrom;
    (void)lineShift;
    _deltaChainLen = 0;
  }

  _versions.push_back(std::move(snap));
  pruneIfNeeded();
}

/**
 * @brief Capture the initial snapshot for a freshly loaded buffer.
 *
 * Same as capture() but skips the comparison with previous snapshot
 * (there is no previous snapshot when loading a file).
 *
 * @param lines The initial line vector from the loaded buffer.
 * @param cursor The cursor position (typically {0, 0}).
 */
void VersionHistory::captureInitial(std::vector<LinePtr> const& lines, BufferPoint cursor) {
  VersionSnapshot snap;
  snap.timestamp = std::chrono::system_clock::now();
  snap.cursor = cursor;
  // Just share the LinePtrs from the live buffer. Buffer::mutLine
  // detaches on first write, so the snapshot's LinePtrs remain valid
  // pointers to the original immutable string content.
  snap.lines = lines;
  _versions.push_back(std::move(snap));
  _deltaChainLen = 0;
}

/**
 * @brief Materialize a delta snapshot into a full snapshot.
 *
 * If the snapshot at @p idx is a delta (stored as patches), this walks back
 * through the delta chain to find the base full snapshot, then applies all
 * intermediate patches to reconstruct the complete line vector.
 *
 * Uses lazy materialization - deltas are only resolved when accessed.
 *
 * @param idx The index of the snapshot to materialize.
 */
void VersionHistory::materialize(std::size_t idx) const {
  if (idx >= _versions.size() || !_versions[idx].isDelta())
    return;

  std::size_t base = idx;
  while (base > 0 && _versions[base].isDelta())
    --base;

  // Value-copy the vector of LinePtr; this aliases the inner
  // shared_ptr<string const> handles rather than deep-copying their
  // string contents. Aliasing is safe because the strings are
  // immutable (shared_ptr<string const>).
  auto lines = _versions[base].lines;
  for (std::size_t i = base + 1; i <= idx; ++i)
    for (auto const& p : _versions[i].patches)
      lines[p.line] = p.content;
  _versions[idx].lines = std::move(lines);
}

/**
 * @brief Drop all snapshots after the given index.
 *
 * Used when committing a history-mode rewind. Resets the delta chain length
 * counter and updates it based on trailing deltas in the remaining history.
 *
 * @param idx The index after which to truncate (keeps snapshots [0, idx]).
 */
void VersionHistory::truncateAfter(std::size_t idx) {
  if (idx + 1 < _versions.size())
    _versions.erase(_versions.begin() + idx + 1, _versions.end());
  _deltaChainLen = 0;
  for (auto it = _versions.rbegin(); it != _versions.rend() && it->isDelta(); ++it)
    ++_deltaChainLen;
}

/**
 * @brief Prune the history if it exceeds the maximum version count.
 *
 * When history size exceeds kMaxVersions, thins every other entry in the
 * oldest kThinCount slots. Merges dropped delta patches into their successors
 * to maintain delta chain validity without materializing full snapshots.
 * After pruning, enforces that no delta chain exceeds kMaxDeltaChain length.
 */
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

  // Pruning can leave delta chains longer than kMaxDeltaChain: capture
  // enforces the bound, but merging dropped patches into successors
  // grows interior chains. Walk the rebuilt deque and force-materialize
  // any delta whose distance from the last full snapshot exceeds the
  // bound, so `at()` never walks beyond kMaxDeltaChain entries.
  std::size_t chain = 0;
  for (std::size_t i = 0; i < _versions.size(); ++i) {
    if (!_versions[i].isDelta()) {
      chain = 0;
      continue;
    }
    ++chain;
    if (chain > kMaxDeltaChain) {
      materialize(i);
      chain = 0;
    }
  }

  _deltaChainLen = 0;
  for (auto it = _versions.rbegin(); it != _versions.rend() && it->isDelta(); ++it)
    ++_deltaChainLen;
}
