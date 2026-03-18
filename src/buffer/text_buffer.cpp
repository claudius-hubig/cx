#include "text_buffer.hpp"

#include <fstream>
#include <iterator>

TextBuffer::TextBuffer(std::string name) : Buffer(std::move(name)) {}

TextBuffer::TextBuffer(std::string name, std::filesystem::path const& path)
    : Buffer(std::move(name)), _path(path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return;
  // Read file into flat buffer, then split into lines.
  std::vector<char> data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  if (data.empty())
    return; // _lines stays as {{}}, correct for empty file.

  _lines.clear();
  _lines.emplace_back();
  for (char c : data) {
    if (c == '\n') {
      _lines.emplace_back(); // start a new line
    } else {
      _lines.back().push_back(c);
    }
  }
}

void TextBuffer::setPath(std::filesystem::path p) {
  _path = std::move(p);
  _name = _path->string();
}

bool TextBuffer::save() {
  if (!_path)
    return false;
  std::ofstream file(*_path, std::ios::binary);
  if (!file)
    return false;
  for (std::size_t i = 0; i < _lines.size(); ++i) {
    if (!_lines[i].empty())
      file.write(_lines[i].data(), static_cast<std::streamsize>(_lines[i].size()));
    if (i + 1 < _lines.size())
      file.put('\n');
  }
  if (file.good()) {
    _modified = false;
    return true;
  }
  return false;
}
