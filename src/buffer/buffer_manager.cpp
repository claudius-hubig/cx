#include "buffer_manager.hpp"
#include "buffer/buffer_sink.hpp"

#include <algorithm>

BufferManager::BufferManager(BufferSink& sink)
    : _helpBuffer(std::make_unique<HelpBuffer>()), _sink(sink) {
  auto buf = std::make_unique<TextBuffer>("*1*");
  auto& ref = *buf;
  _buffers.emplace("*1*", std::move(buf));
  _mru.push_back("*1*");
  refreshDisplayNames();
  _sink.setBuffer(ref);
}

Buffer& BufferManager::open(std::filesystem::path const& path) {
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
  auto key = canonical.string();
  auto it = _buffers.find(key);
  if (it != _buffers.end()) {
    touch(key);
    return *it->second;
  }
  auto buf = std::make_unique<TextBuffer>(key, canonical);
  auto& ref = *buf;
  _buffers.emplace(key, std::move(buf));
  refreshDisplayNames();
  touch(key);
  return ref;
}

BufferListBuffer& BufferManager::bufferListBuffer() {
  // Text buffer names go into the trie; dir buffer names are
  // merged as selectable directory headers.
  std::vector<std::string> names;
  names.reserve(_buffers.size());
  for (auto& [name, _] : _buffers)
    names.push_back(name);
  std::sort(names.begin(), names.end());

  std::vector<std::string> dirNames;
  dirNames.reserve(_dirBuffers.size());
  for (auto& [name, _] : _dirBuffers)
    dirNames.push_back(name);

  auto target = secondMRU();

  if (!_bufferListBuffer) {
    _bufferListBuffer =
        std::make_unique<BufferListBuffer>(std::move(names), std::move(dirNames), target);
  } else {
    auto hint = _bufferListBuffer->refresh(std::move(names), std::move(dirNames), target);
    _bufferListBuffer->setSavedView({hint.point, hint.viewFrom, std::nullopt});
  }
  return *_bufferListBuffer;
}

PasteRingBuffer& BufferManager::pasteRingBuffer(PasteRing const& pr) {
  if (!_pasteRingBuffer)
    _pasteRingBuffer = std::make_unique<PasteRingBuffer>();
  auto hint = _pasteRingBuffer->refresh(pr);
  _pasteRingBuffer->setSavedView({hint.point, hint.viewFrom, std::nullopt});
  return *_pasteRingBuffer;
}

DirBuffer& BufferManager::openDir(std::filesystem::path const& dir) {
  auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(dir));
  auto key = canonical.string();
  auto it = _dirBuffers.find(key);
  if (it != _dirBuffers.end()) {
    touch(key);
    return *it->second;
  }
  auto buf = std::make_unique<DirBuffer>(canonical);
  auto& ref = *buf;
  _dirBuffers.emplace(key, std::move(buf));
  refreshDisplayNames();
  touch(key);
  return ref;
}

Buffer& BufferManager::create(std::string name) {
  auto [it, inserted] = _buffers.emplace(name, nullptr);
  if (!inserted)
    return *it->second;
  auto key = it->first;
  it->second = std::make_unique<TextBuffer>(std::move(name));
  refreshDisplayNames();
  touch(key);
  return *it->second;
}

Buffer* BufferManager::find(std::string const& name) {
  auto it = _buffers.find(name);
  if (it != _buffers.end())
    return it->second.get();
  auto dit = _dirBuffers.find(name);
  if (dit != _dirBuffers.end())
    return dit->second.get();
  return nullptr;
}

std::vector<std::string> BufferManager::bufferNames() const {
  std::vector<std::string> names;
  names.reserve(_buffers.size() + _dirBuffers.size());
  for (auto& [name, _] : _buffers)
    names.push_back(name);
  for (auto& [name, _] : _dirBuffers)
    names.push_back(name);
  std::sort(names.begin(), names.end());
  return names;
}

std::string BufferManager::displayName(std::string const& name) const {
  auto it = _displayNames.find(name);
  if (it != _displayNames.end())
    return it->second;
  return name;
}

void BufferManager::refreshDisplayNames() {
  _displayNames.clear();
  _displayNames.reserve(_buffers.size() + _dirBuffers.size());
  // Count filename occurrences to detect ambiguity.
  std::unordered_map<std::string, int> fnCount;
  for (auto& [name, _] : _buffers) {
    auto fn = std::filesystem::path(name).filename().string();
    if (!fn.empty() && fn != name)
      fnCount[fn]++;
  }
  for (auto& [name, _] : _buffers) {
    auto fn = std::filesystem::path(name).filename().string();
    if (!fn.empty() && fn != name && fnCount[fn] == 1)
      _displayNames[name] = fn;
    else
      _displayNames[name] = name;
  }
  // Dir buffers use their path as display name.
  for (auto& [name, _] : _dirBuffers)
    _displayNames[name] = name;
}

std::vector<std::pair<std::string, std::string>>
BufferManager::completions(std::string const& prefix) const {
  std::vector<std::pair<std::string, std::string>> result;

  // Match against open buffers using cached display names.
  for (auto& [name, _] : _buffers) {
    auto dn = displayName(name);
    if (dn.starts_with(prefix))
      result.push_back({dn, name});
    else if (name.starts_with(prefix))
      result.push_back({name, name});
    else {
      // Also match against the bare filename (covers the ambiguous
      // case where displayName returns the full path).
      auto fn = std::filesystem::path(name).filename().string();
      if (!fn.empty() && fn != dn && fn.starts_with(prefix))
        result.push_back({name, name});
    }
  }

  // Match against files on disk (cached by directory).
  if (!prefix.empty()) {
    std::filesystem::path dir = ".";
    std::string filePrefix = prefix;
    auto lastSlash = prefix.rfind('/');
    if (lastSlash != std::string::npos) {
      auto dirStr = prefix.substr(0, lastSlash);
      dir = dirStr.empty() ? std::filesystem::path("/") : std::filesystem::path(dirStr);
      filePrefix = prefix.substr(lastSlash + 1);
    }

    auto dirStr = dir.string();
    if (_dirCache.dir != dirStr) {
      _dirCache.dir = dirStr;
      _dirCache.entries.clear();
      std::error_code ec;
      for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        auto filename = entry.path().filename().string();
        if (!filename.starts_with("."))
          _dirCache.entries.push_back({filename, entry.is_directory(ec)});
      }
    }

    // Canonicalize the directory once, not per-entry.
    auto canonDir = std::filesystem::weakly_canonical(std::filesystem::absolute(dir));

    for (auto& [filename, isDir] : _dirCache.entries) {
      if (!filename.starts_with(filePrefix))
        continue;

      std::string matchStr;
      if (lastSlash != std::string::npos)
        matchStr = prefix.substr(0, lastSlash + 1) + filename;
      else
        matchStr = filename;

      if (isDir)
        matchStr += "/";

      // If this file is already an open buffer, use its buffer name
      // so that find() resolves to the existing buffer.
      auto canonStr = (canonDir / filename).string();
      auto bufIt = _buffers.find(canonStr);
      result.push_back({matchStr, bufIt != _buffers.end() ? canonStr : matchStr});
    }
  }

  std::sort(result.begin(), result.end());
  // Deduplicate by match string.
  auto last = std::unique(result.begin(), result.end(),
                          [](auto const& a, auto const& b) { return a.first == b.first; });
  result.erase(last, result.end());
  return result;
}

void BufferManager::rename(std::string const& oldName, std::filesystem::path newPath) {
  auto it = _buffers.find(oldName);
  if (it == _buffers.end())
    return;
  auto buf = std::move(it->second);
  _buffers.erase(it);
  buf->setPath(std::move(newPath));
  auto newName = buf->name();
  _buffers.emplace(newName, std::move(buf));
  refreshDisplayNames();
  // Update MRU.
  auto mi = std::find(_mru.begin(), _mru.end(), oldName);
  if (mi != _mru.end())
    *mi = newName;
}

Buffer& BufferManager::kill(std::string name) {
  _buffers.erase(name);
  _dirBuffers.erase(name);
  auto it = std::find(_mru.begin(), _mru.end(), name);
  if (it != _mru.end())
    _mru.erase(it);
  refreshDisplayNames();
  if (_buffers.empty() && _dirBuffers.empty()) {
    auto buf = std::make_unique<TextBuffer>("*1*");
    auto& ref = *buf;
    _buffers.emplace("*1*", std::move(buf));
    _mru.push_back("*1*");
    return ref;
  }
  // Return most recently used buffer.
  for (auto& mruName : _mru) {
    if (auto* buf = find(mruName))
      return *buf;
  }
  // Fallback.
  if (!_buffers.empty())
    return *_buffers.begin()->second;
  return *_dirBuffers.begin()->second;
}

void BufferManager::touch(std::string const& name) {
  // Move name to front of MRU list.
  auto it = std::find(_mru.begin(), _mru.end(), name);
  if (it != _mru.end())
    _mru.erase(it);
  _mru.insert(_mru.begin(), name);
}

std::string BufferManager::secondMRU() const {
  if (_mru.size() >= 2)
    return _mru[1];
  return {};
}
