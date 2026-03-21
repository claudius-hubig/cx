#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Global ring buffer for cut/copied text. Stores entries as std::string
// with embedded '\n' for line breaks. Fixed capacity (default 20).
// Index 0 = most recent.
class PasteRing {
public:
  static constexpr std::size_t kDefaultCapacity = 20;

  void push(std::string text) {
    _entries.insert(_entries.begin(), std::move(text));
    if (_entries.size() > _capacity)
      _entries.pop_back();
  }

  std::string const& at(std::size_t idx) const { return _entries.at(idx); }

  std::size_t size() const { return _entries.size(); }
  bool empty() const { return _entries.empty(); }

private:
  std::vector<std::string> _entries;
  std::size_t _capacity = kDefaultCapacity;
};
