#pragma once

/**
 * @file bookmarks.hpp
 * @brief Bookmarks — session-wide named positions keyed by a single
 *        character, independent of any buffer or view.
 */

#include "types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** @brief One saved position: the buffer it belongs to and the point within it. */
struct Bookmark {
  std::string bufferName;
  BufferPoint point;
};

/**
 * @brief Session-wide bookmark slots keyed by a single char in `[0-9a-z]`.
 *
 * Global across buffers (jumping switches the focused panel's buffer if
 * needed) and session-only (never persisted). Owned by `Screen` rather
 * than any `Buffer` or `BufferView`: bookmarks jump across buffers, and
 * views are transient (dropped on every buffer switch), so neither is a
 * valid owner.
 */
class Bookmarks {
public:
  /** @brief True if @p c is a valid slot name: '0'-'9' or 'a'-'z'. */
  static bool validSlot(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); }

  /** @brief Set (or silently overwrite) @p slot to @p bufferName / @p p. */
  void set(char slot, std::string bufferName, BufferPoint p) {
    _slots[slot] = Bookmark{std::move(bufferName), p};
  }

  /** @brief Look up @p slot. */
  std::optional<Bookmark> get(char slot) const {
    auto it = _slots.find(slot);
    if (it == _slots.end())
      return std::nullopt;
    return it->second;
  }

  /** @brief True if @p slot is occupied. Cheaper than get() when the value is unused. */
  bool contains(char slot) const { return _slots.contains(slot); }

  /** @brief Remove @p slot, if occupied. */
  void erase(char slot) { _slots.erase(slot); }

  /**
   * @brief Re-key every bookmark whose @c bufferName is @p from to @p to.
   *
   * Called after a buffer is renamed (Save-As) so its bookmarks keep
   * pointing at it under the new name.
   */
  void renameBuffer(std::string_view from, std::string_view to) {
    for (auto& [slot, mark] : _slots)
      if (mark.bufferName == from)
        mark.bufferName = std::string(to);
  }

  /** @brief Occupied slots in sorted order, for the jump-prompt hint. */
  std::vector<std::pair<char, Bookmark>> list() const {
    std::vector<std::pair<char, Bookmark>> out;
    out.reserve(_slots.size());
    for (auto& [slot, mark] : _slots)
      out.emplace_back(slot, mark);
    return out;
  }

private:
  std::map<char, Bookmark> _slots; // ordered → hint lists slots sorted
};
