#include "text_buffer.hpp"

#include <expected>
#include <filesystem>
#include <fstream>

TextBuffer::TextBuffer(std::string name) : Buffer(std::move(name)) { detectMode(); }

TextBuffer::TextBuffer(std::string name, std::filesystem::path const& path)
    : Buffer(std::move(name)), _path(path) {
  detectMode();
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return;
  // Stream the file in chunks to avoid a full intermediate copy.
  _lines.clear();
  _lines.emplace_back();
  char buf[8192];
  bool anyData = false;
  while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
    auto n = static_cast<std::size_t>(file.gcount());
    anyData = true;
    for (std::size_t i = 0; i < n; ++i) {
      if (buf[i] == '\n')
        _lines.emplace_back();
      else
        _lines.back().push_back(buf[i]);
    }
  }
  if (!anyData) {
    // Empty file: restore the default single empty line.
    _lines = {{}};
  }
}

void TextBuffer::detectMode() {
  if (!_path)
    return;
  auto ext = _path->extension().string();
  if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".ixx" ||
      ext == ".cxx")
    _mode = BufferMode::Cpp;
  else
    _mode = BufferMode::Text;
}

void TextBuffer::setPath(std::filesystem::path p) {
  _path = std::move(p);
  _name = _path->string();
  detectMode();
}

std::expected<void, std::string> TextBuffer::save() {
  if (!_path)
    return std::unexpected("buffer has no path");
  std::ofstream file(*_path, std::ios::binary);
  if (!file)
    return std::unexpected("cannot open " + _path->string());
  for (std::size_t i = 0; i < _lines.size(); ++i) {
    if (!_lines[i].empty())
      file.write(_lines[i].data(), static_cast<std::streamsize>(_lines[i].size()));
    if (i + 1 < _lines.size())
      file.put('\n');
  }
  if (!file.good())
    return std::unexpected("write error for " + _path->string());
  _modified = false;
  return {};
}

std::expected<void, std::string> TextBuffer::revert() {
  if (!_path)
    return std::unexpected("buffer has no path");
  std::ifstream file(*_path, std::ios::binary);
  if (!file)
    return std::unexpected("cannot open " + _path->string());

  _lines.clear();
  _lines.emplace_back();
  char buf[8192];
  while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
    auto n = static_cast<std::size_t>(file.gcount());
    for (std::size_t i = 0; i < n; ++i) {
      if (buf[i] == '\n')
        _lines.emplace_back();
      else
        _lines.back().push_back(buf[i]);
    }
  }
  _point = {};
  _mark.reset();
  _viewFrom = {};
  _desiredCol.reset();
  _modified = false;
  return {};
}
