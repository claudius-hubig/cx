#pragma once

/**
 * @file paste_ring.hpp
 * @brief PasteRing — a fixed-capacity ring buffer holding recently cut or
 *        copied text for paste and paste-cycle.
 */

#include <cstddef>
#include <deque>
#include <string>

/**
 * @brief Ring buffer of cut/copied text entries.
 *
 * Entries are stored as std::string with embedded '\n' for line breaks.
 * Index 0 is always the most recent entry. The ring grows up to its
 * capacity, after which push() evicts the oldest entry.
 *
 * PasteCycle indices in Screen take size() modulo and so survive
 * evictions; clients must not assume index stability across pushes.
 */
class PasteRing {
public:
  /** @brief Default ring capacity. */
  static constexpr std::size_t kDefaultCapacity = 20;

  /** @brief Construct a ring at the default capacity. */
  PasteRing() = default;

  /**
   * @brief Construct a ring with an explicit capacity.
   *
   * Exists so tests can exercise capacity-edge behaviour without
   * depending on the default value.
   * @param capacity Maximum number of entries retained.
   */
  explicit PasteRing(std::size_t capacity) : _capacity(capacity) {}

  /**
   * @brief Push a new entry at index 0, evicting the oldest if full.
   * @param text The text to store (ownership taken).
   */
  void push(std::string text) {
    _entries.push_front(std::move(text));
    if (_entries.size() > _capacity)
      _entries.pop_back();
  }

  /**
   * @brief Access the entry at @p idx (0 = most recent).
   * @throws std::out_of_range if @p idx is out of bounds.
   */
  std::string const& at(std::size_t idx) const { return _entries.at(idx); }

  /** @brief Number of entries currently held. */
  std::size_t size() const { return _entries.size(); }
  /** @brief True if the ring holds no entries. */
  bool empty() const { return _entries.empty(); }

private:
  std::deque<std::string> _entries;
  std::size_t _capacity = kDefaultCapacity;
};
