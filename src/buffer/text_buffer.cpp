#include "text_buffer.hpp"

#include <cstring>
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
  _history.captureInitial(_lines, {0, 0});
}

void TextBuffer::loadFromFile(std::filesystem::path const& path) {
  // Reset before any early return so that revert() to a now-empty or
  // unreadable file doesn't leave stale conflict-marker overlays from
  // before, and so loadFromFile is idempotent on its bookkeeping.
  _hasConflicts = false;
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    _lineEndingStyle = LineEndingStyle::Unix;
    return;
  }
  // Use the error_code overload so a transient EACCES or stale NFS
  // handle is treated as "open empty" (consistent with the !file
  // branch above) rather than throwing out of a constructor or revert.
  std::error_code ec;
  auto fileSize = std::filesystem::file_size(path, ec);
  if (ec) {
    _lineEndingStyle = LineEndingStyle::Unix;
    return;
  }
  if (fileSize == 0) {
    _lines = {{}};
    _lineEndingStyle = LineEndingStyle::Unix;
    return;
  }
  std::string data(fileSize, '\0');
  file.read(data.data(), static_cast<std::streamsize>(fileSize));
  auto bytesRead = static_cast<std::size_t>(file.gcount());
  data.resize(bytesRead);

  _lines.clear();
  _lines.reserve(bytesRead / 40 + 1);
  char const* p = data.data();
  char const* end = p + bytesRead;
  while (p < end) {
    auto* nl = static_cast<char const*>(std::memchr(p, '\n', end - p));
    if (nl) {
      _lines.emplace_back(p, nl - p);
      if (!_hasConflicts && isConflictMarker(_lines.back()))
        _hasConflicts = true;
      p = nl + 1;
    } else {
      _lines.emplace_back(p, end - p);
      if (!_hasConflicts && isConflictMarker(_lines.back()))
        _hasConflicts = true;
      p = end;
    }
  }
  if (_lines.empty())
    _lines.emplace_back();
  if (bytesRead > 0 && data[bytesRead - 1] == '\n')
    _lines.emplace_back();

  // Detect line ending style from the raw data.
  bool hasCr = std::memchr(data.data(), '\r', bytesRead) != nullptr;
  bool hasLoneLf = false, hasCrlf = false, hasLoneCr = false;
  if (hasCr) {
    for (std::size_t i = 0; i < bytesRead; ++i) {
      if (data[i] == '\n') {
        if (i > 0 && data[i - 1] == '\r')
          hasCrlf = true;
        else
          hasLoneLf = true;
      } else if (data[i] == '\r') {
        if (i + 1 >= bytesRead || data[i + 1] != '\n')
          hasLoneCr = true;
      }
    }
  } else {
    hasLoneLf = (std::memchr(data.data(), '\n', bytesRead) != nullptr);
  }

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
    _mode = BufferMode::Dif;
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
  // Append (rather than clear) so the user can undo a revert. The
  // capture takes the SIZE_MAX/full-diff branch and is therefore O(N)
  // line comparisons against the previous snapshot — acceptable for
  // an interactive operation and the cost of preserving undo across
  // reverts.
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
