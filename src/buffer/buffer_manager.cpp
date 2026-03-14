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
    auto key = path.string();
    auto it = _buffers.find(key);
    if (it != _buffers.end())
        return *it->second;
    auto buf = std::make_unique<TextBuffer>(key, path);
    auto& ref = *buf;
    _buffers.emplace(key, std::move(buf));
    return ref;
}

BufferListBuffer& BufferManager::bufferListBuffer() {
    auto names = bufferNames();
    auto target = secondMRU();

    // Find which sorted index the target name lands on.
    std::size_t startLine = 0;
    if (!target.empty()) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] == target) {
                startLine = i;
                break;
            }
        }
    }

    if (!_bufferListBuffer)
        _bufferListBuffer = std::make_unique<BufferListBuffer>(std::move(names), startLine);
    else
        _bufferListBuffer->refresh(std::move(names), startLine);
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
