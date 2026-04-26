#include "text_buffer.hpp"

#include <expected>
#include <filesystem>
#include <fstream>
#include <utility>

TextBuffer::TextBuffer(std::string name) : Buffer(std::move(name)) {
  detectMode();
  _history.capture(_lines, {0, 0});
}

TextBuffer::TextBuffer(std::string name, std::filesystem::path const& path)
    : Buffer(std::move(name)), _path(path) {
  detectMode();
  loadFromFile(path);
  _history.capture(_lines, {0, 0});
}

void TextBuffer::loadFromFile(std::filesystem::path const& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    _lineEndingStyle = LineEndingStyle::Unix;
    return;
  }
  // Stream the file in chunks to avoid a full intermediate copy. Lines
  // are split on '\n'; '\r' bytes are left in place so the user sees the
  // file verbatim. We scan for line ending style in the same pass so
  // the caller can decide whether to prompt for translation.
  _lines.clear();
  _lines.emplace_back();
  bool hasLoneLf = false, hasCrlf = false, hasLoneCr = false;
  bool prevCr = false;
  char buf[8192];
  bool anyData = false;
  while (file.read(buf, sizeof(buf)) || file.gcount() > 0) {
    auto n = static_cast<std::size_t>(file.gcount());
    anyData = true;
    for (std::size_t i = 0; i < n; ++i) {
      char c = buf[i];
      if (c == '\n') {
        if (prevCr)
          hasCrlf = true;
        else
          hasLoneLf = true;
        prevCr = false;
        _lines.emplace_back();
      } else {
        if (prevCr)
          hasLoneCr = true; // '\r' was not followed by '\n'
        prevCr = (c == '\r');
        _lines.back().push_back(c);
      }
    }
  }
  if (prevCr)
    hasLoneCr = true;
  if (!anyData)
    _lines = {{}};

  scanForConflicts();

  int styleCount = (hasLoneLf ? 1 : 0) + (hasCrlf ? 1 : 0) + (hasLoneCr ? 1 : 0);
  if (styleCount > 1)
    _lineEndingStyle = LineEndingStyle::Mixed;
  else if (hasCrlf)
    _lineEndingStyle = LineEndingStyle::Crlf;
  else if (hasLoneCr)
    _lineEndingStyle = LineEndingStyle::Cr;
  else
    _lineEndingStyle = LineEndingStyle::Unix;
}

void TextBuffer::normalizeLineEndings() {
  if (_lineEndingStyle == LineEndingStyle::Unix || _lineEndingStyle == LineEndingStyle::Mixed)
    return;
  if (_lineEndingStyle == LineEndingStyle::Crlf) {
    // Strip the trailing '\r' that every line carries for a pure CRLF file.
    for (auto& line : _lines) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
    }
  } else {
    // Cr: the file has no '\n' at all, so every '\r' sits inside a line.
    // Re-split each line on '\r' to form new lines.
    std::vector<std::string> newLines;
    newLines.reserve(_lines.size());
    for (auto& line : _lines) {
      newLines.emplace_back();
      for (char c : line) {
        if (c == '\r')
          newLines.emplace_back();
        else
          newLines.back().push_back(c);
      }
    }
    _lines = std::move(newLines);
  }
  _lineEndingStyle = LineEndingStyle::Unix;
  _modified = true;
  ++_docVersion;
  _synCacheValidUpto = 0;
  _history.capture(_lines, {0, 0});
}

void TextBuffer::detectMode() {
  if (!_path)
    return;
  auto ext = _path->extension().string();
  if (ext == ".c" || ext == ".h" || ext == ".cpp" || ext == ".hpp" || ext == ".ixx" ||
      ext == ".cxx")
    _mode = BufferMode::Cpp;
  else if (ext == ".md")
    _mode = BufferMode::Md;
  else if (ext == ".json")
    _mode = BufferMode::Jsn;
  else if (ext == ".diff" || ext == ".patch")
    _mode = BufferMode::Diff;
  else
    _mode = BufferMode::Txt;
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
  scanForConflicts();
  return {};
}

std::expected<void, std::string> TextBuffer::revert() {
  if (!_path)
    return std::unexpected("buffer has no path");
  if (!std::ifstream(*_path, std::ios::binary))
    return std::unexpected("cannot open " + _path->string());
  loadFromFile(*_path);
  _modified = false;
  ++_docVersion;
  _synCacheValidUpto = 0;
  scanForConflicts();
  _history.capture(_lines, {0, 0});
  return {};
}

void TextBuffer::applySnapshot(VersionSnapshot const& s) {
  _lines.clear();
  _lines.reserve(s.lines.size());
  for (auto const& lp : s.lines)
    _lines.push_back(*lp);
  if (_lines.empty())
    _lines.emplace_back();
  _modified = true;
  ++_docVersion;
  _synCacheValidUpto = 0;
}
