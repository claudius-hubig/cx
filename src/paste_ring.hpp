#pragma once

#include <cstddef>
#include <deque>
#include <string>

// Global ring buffer for cut/copied text. Stores entries as std::string
// with embedded '\n' for line breaks. Fixed capacity (default 20).
// Index 0 = most recent.
//
// The ring never shrinks past evictions: push() either grows up to
// _capacity or pops the oldest. PasteCycle indices in Screen mod by
// size() and so survive evictions; clients must not assume index
// stability across pushes.
class PasteRing {
public:
  static constexpr std::size_t kDefaultCapacity = 20;

  // Default-constructible at the standard capacity. The explicit
  // overload exists so tests can exercise capacity-edge behaviour
  // without depending on the default value.
  PasteRing() = default;
  explicit PasteRing(std::size_t capacity) : _capacity(capacity) {}

  void push(std::string text) {
    _entries.push_front(std::move(text));
    if (_entries.size() > _capacity)
      _entries.pop_back();
  }

  std::string const& at(std::size_t idx) const { return _entries.at(idx); }

  std::size_t size() const { return _entries.size(); }
  bool empty() const { return _entries.empty(); }

private:
  std::deque<std::string> _entries;
  std::size_t _capacity = kDefaultCapacity;
};
