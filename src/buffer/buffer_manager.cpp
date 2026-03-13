#include "buffer_manager.hpp"
#include "terminal/screen.hpp"

BufferManager::BufferManager(Screen& screen)
    : _helpBuffer(std::make_unique<HelpBuffer>()), _screen(screen) {
    auto buf = std::make_unique<TextBuffer>("*1*");
    auto& ref = *buf;
    _buffers.emplace("*1*", std::move(buf));
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
