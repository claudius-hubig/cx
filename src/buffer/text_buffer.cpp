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
  loadFromFile(path);
  // After load so the shebang fallback can inspect the first line.
  detectMode();
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
    _lines.clear();
    _lines.push_back(makeLine());
    _lineEndingStyle = LineEndingStyle::Unix;
    return;
  }
  std::string data(fileSize, '\0');
  file.read(data.data(), static_cast<std::streamsize>(fileSize));
  auto bytesRead = static_cast<std::size_t>(file.gcount());
  data.resize(bytesRead);

  _lines.clear();
  _lines.reserve(bytesRead / 40 + 1);
  // Helper to push a fresh line from a [begin, end) byte range.
  auto pushLine = [&](char const* begin, char const* end) {
    _lines.push_back(makeLine(std::string_view(begin, end - begin)));
  };
  char const* p = data.data();
  // Single pass: split lines on '\n' and classify each line ending in
  // place by inspecting the preceding byte (data[nl-1]) for '\r'. A
  // separate pass tracking lone-CR bytes is unnecessary because every
  // '\r' that is not immediately followed by '\n' is by definition not
  // part of a CRLF, and any CR that is followed by '\n' is captured as
  // CRLF when we visit that '\n'. Lone CRs are detected by walking
  // each line we collect (which preserves embedded '\r').
  char const* end = p + bytesRead;
  bool hasLoneLf = false, hasCrlf = false, hasLoneCr = false;
  while (p < end) {
    auto* nl = static_cast<char const*>(std::memchr(p, '\n', end - p));
    char const* lineEnd = nl ? nl : end;
    pushLine(p, lineEnd);
    auto const& just = *_lines.back();
    if (!_hasConflicts && isConflictMarker(just))
      _hasConflicts = true;
    if (nl) {
      if (nl > p && nl[-1] == '\r')
        hasCrlf = true;
      else
        hasLoneLf = true;
      // Any '\r' inside this line (i.e. not the trailing CRLF marker
      // which already lives in `just` if present) is a lone CR.
      std::size_t scanLimit =
          (hasCrlf && !just.empty() && just.back() == '\r') ? just.size() - 1 : just.size();
      if (!hasLoneCr && std::memchr(just.data(), '\r', scanLimit) != nullptr)
        hasLoneCr = true;
      p = nl + 1;
    } else {
      // Final partial line (no trailing '\n').
      if (!hasLoneCr && std::memchr(just.data(), '\r', just.size()) != nullptr)
        hasLoneCr = true;
      p = end;
    }
  }
  if (_lines.empty())
    _lines.push_back(makeLine());
  // Trailing-empty-line invariant (see Buffer::_lines doc in buffer.hpp:
  // "A file ending with '\n' is represented by an empty trailing entry"):
  // the save site at line ~158 mirrors this by writing a '\n' between
  // every adjacent pair of lines, so the empty trailing entry round-trips
  // back to a file that ends in '\n'.
  if (bytesRead > 0 && data[bytesRead - 1] == '\n')
    _lines.push_back(makeLine());

  int styleCount = (hasLoneLf ? 1 : 0) + (hasCrlf ? 1 : 0) + (hasLoneCr ? 1 : 0);
  if (styleCount > 1)
    _lineEndingStyle = LineEndingStyle::Mixed;
  else if (hasCrlf)
    _lineEndingStyle = LineEndingStyle::Crlf;
  else if (hasLoneCr)
    _lineEndingStyle = LineEndingStyle::Cr;
  else
    _lineEndingStyle = LineEndingStyle::Unix;
  captureDiskStamp();
}

void TextBuffer::captureDiskStamp() {
  if (!_path) {
    _diskMtime.reset();
    _diskSize.reset();
    return;
  }
  std::error_code ec;
  auto mt = std::filesystem::last_write_time(*_path, ec);
  if (ec) {
    _diskMtime.reset();
    _diskSize.reset();
    return;
  }
  std::error_code ec2;
  auto sz = std::filesystem::file_size(*_path, ec2);
  _diskMtime = mt;
  _diskSize = ec2 ? std::optional<std::uintmax_t>{} : std::optional<std::uintmax_t>{sz};
}

bool TextBuffer::changedOnDisk() const {
  if (!_path || !_diskMtime)
    return false;
  std::error_code ec;
  auto mt = std::filesystem::last_write_time(*_path, ec);
  if (ec)
    return false;
  if (mt != *_diskMtime)
    return true;
  std::error_code ec2;
  auto sz = std::filesystem::file_size(*_path, ec2);
  if (ec2 || !_diskSize)
    return false;
  return sz != *_diskSize;
}

void TextBuffer::normalizeLineEndings() {
  if (_lineEndingStyle == LineEndingStyle::Unix || _lineEndingStyle == LineEndingStyle::Mixed)
    return;
  if (_lineEndingStyle == LineEndingStyle::Crlf) {
    // Strip the trailing '\r' that every line carries for a pure CRLF file.
    for (std::size_t i = 0; i < _lines.size(); ++i) {
      if (!_lines[i]->empty() && _lines[i]->back() == '\r')
        mutLine(i).pop_back();
    }
  } else {
    // Cr: the file has no '\n' at all, so every '\r' sits inside a line.
    // Re-split each line on '\r' to form new lines.
    std::vector<LinePtr> newLines;
    newLines.reserve(_lines.size());
    std::string accum;
    for (auto const& linePtr : _lines) {
      accum.clear();
      for (char c : *linePtr) {
        if (c == '\r') {
          newLines.push_back(makeLine(std::move(accum)));
          accum.clear();
        } else {
          accum.push_back(c);
        }
      }
      newLines.push_back(makeLine(std::move(accum)));
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
  else if (ext == ".sh" || ext == ".bash" || ext == ".zsh" || ext == ".ksh")
    _mode = BufferMode::Sh;
  else if (ext == ".yml" || ext == ".yaml")
    _mode = BufferMode::Yml;
  else if (ext == ".py" || ext == ".pyi" || ext == ".pyw")
    _mode = BufferMode::Py;
  else
    _mode = BufferMode::Txt;

  // Shebang fallback: when the extension didn't pin a mode, sniff the
  // interpreter from a leading `#!` line (only line 0 is inspected).
  if (_mode == BufferMode::Txt && !_lines.empty()) {
    auto const& first = *_lines[0];
    if (first.size() >= 2 && first[0] == '#' && first[1] == '!') {
      auto has = [&](std::string_view needle) { return first.find(needle) != std::string::npos; };
      if (has("bash") || has("zsh") || has("ksh") || has("dash") || has("/sh") || has(" sh"))
        _mode = BufferMode::Sh;
      else if (has("python"))
        _mode = BufferMode::Py;
    }
  }
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
  // Mirrors the load site (see loadFromFile): a '\n' is written between
  // every adjacent pair of lines but never after the last, so a buffer
  // whose representation has an empty trailing entry (per the invariant
  // documented in buffer.hpp) round-trips to a file ending in '\n'.
  for (std::size_t i = 0; i < _lines.size(); ++i) {
    auto const& line = *_lines[i];
    if (!line.empty())
      file.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (i + 1 < _lines.size())
      file.put('\n');
  }
  if (!file.good())
    return std::unexpected("write error for " + _path->string());
  // Close and flush before stat so the stamp reflects the bytes we just
  // wrote rather than a still-buffered zero size or stale mtime.
  file.close();
  _modified = false;
  scanForConflicts();
  captureDiskStamp();
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
  // Adopt the snapshot's LinePtrs directly. Lines unchanged across
  // snapshots share storage; lines changed since the snapshot was
  // taken were already detached on edit (Buffer::mutLine).
  _lines = s.lines;
  if (_lines.empty())
    _lines.push_back(makeLine());
  _modified = true;
  ++_docVersion;
  _synCacheValidUpto = 0;
}
