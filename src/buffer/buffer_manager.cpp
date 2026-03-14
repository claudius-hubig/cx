#include "buffer_manager.hpp"
#include "terminal/screen.hpp"

#include <algorithm>

BufferManager::BufferManager(Screen& screen)
    : _helpBuffer(std::make_unique<HelpBuffer>()), _screen(screen) {
    auto buf = std::make_unique<TextBuffer>("*1*");
    auto& ref = *buf;
    _buffers.emplace("*1*", std::move(buf));
    _mru.push_back("*1*");
    _screen.setBuffer(ref);
}

Buffer& BufferManager::open(std::filesystem::path const& path) {
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path));
    auto key = canonical.string();
    auto it = _buffers.find(key);
    if (it != _buffers.end())
        return *it->second;
    auto buf = std::make_unique<TextBuffer>(key, canonical);
    auto& ref = *buf;
    _buffers.emplace(key, std::move(buf));
    return ref;
}

BufferListBuffer& BufferManager::bufferListBuffer() {
    auto names = bufferNames();
    auto target = secondMRU();

    if (!_bufferListBuffer)
        _bufferListBuffer = std::make_unique<BufferListBuffer>(std::move(names), target);
    else
        _bufferListBuffer->refresh(std::move(names), target);
    return *_bufferListBuffer;
}

Buffer& BufferManager::create(std::string name) {
    auto [it, inserted] = _buffers.emplace(name, nullptr);
    if (!inserted)
        return *it->second;
    it->second = std::make_unique<TextBuffer>(std::move(name));
    return *it->second;
}

Buffer* BufferManager::find(std::string const& name) {
    auto it = _buffers.find(name);
    if (it != _buffers.end())
        return it->second.get();
    return nullptr;
}

std::vector<std::string> BufferManager::bufferNames() const {
    std::vector<std::string> names;
    names.reserve(_buffers.size());
    for (auto& [name, _] : _buffers)
        names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

std::string BufferManager::displayName(std::string const& name) const {
    auto filename = std::filesystem::path(name).filename().string();
    if (filename.empty() || filename == name)
        return name;
    // Check if any other buffer shares the same filename.
    for (auto& [other, _] : _buffers)
        if (other != name && std::filesystem::path(other).filename() == filename)
            return name;
    return filename;
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
