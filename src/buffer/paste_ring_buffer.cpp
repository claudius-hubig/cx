#include "paste_ring_buffer.hpp"

PasteRingBuffer::PasteRingBuffer() : Buffer("*paste ring*") {}

void PasteRingBuffer::refresh(PasteRing const& pr) {
  _lines.clear();

  if (pr.empty()) {
    auto sv = std::string_view{"(empty)"};
    _lines.emplace_back(sv.begin(), sv.end());
  } else {
    for (std::size_t i = 0; i < pr.size(); ++i) {
      // Header line.
      std::string header = "--- Entry " + std::to_string(i + 1) + " ---";
      _lines.emplace_back(header.begin(), header.end());

      // Entry content, split on '\n'.
      auto const& text = pr.at(i);
      std::size_t pos = 0;
      while (pos <= text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
          _lines.emplace_back(text.begin() + pos, text.end());
          break;
        }
        _lines.emplace_back(text.begin() + pos, text.begin() + nl);
        pos = nl + 1;
      }

      // Blank line between entries.
      if (i + 1 < pr.size())
        _lines.emplace_back();
    }
  }

  _point = {0, 0};
  _viewFrom = ViewFrom{};
  _desiredCol.reset();
}
