#include "paste_ring_buffer.hpp"

PasteRingBuffer::PasteRingBuffer() : Buffer("*paste ring*") {}

ViewHint PasteRingBuffer::refresh(PasteRing const& pr) {
  _lines.clear();

  if (pr.empty()) {
    _lines.push_back(makeLine(std::string_view{"(empty)"}));
  } else {
    for (std::size_t i = 0; i < pr.size(); ++i) {
      // Header line.
      _lines.push_back(makeLine("--- Entry " + std::to_string(i + 1) + " ---"));

      // Entry content, split on '\n'. Use `<` rather than `<=` so an
      // entry that ends in '\n' does not produce a phantom empty
      // trailing display line. The blank between entries below is
      // sufficient to visually separate adjacent entries.
      auto const& text = pr.at(i);
      std::size_t pos = 0;
      while (pos < text.size()) {
        auto nl = text.find('\n', pos);
        if (nl == std::string::npos) {
          _lines.push_back(makeLine(std::string_view(text.data() + pos, text.size() - pos)));
          break;
        }
        _lines.push_back(makeLine(std::string_view(text.data() + pos, nl - pos)));
        pos = nl + 1;
      }

      // Blank line between entries.
      if (i + 1 < pr.size())
        _lines.push_back(makeLine());
    }
  }

  return ViewHint{{0, 0}, {}};
}
