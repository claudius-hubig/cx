#include "buffer_manager.hpp"
#include "terminal/screen.hpp"

BufferManager::BufferManager(Screen& screen) : _screen(screen) {
    auto [it, _] = _buffers.emplace("*1*", Buffer{"*1*"});
    _screen.setBuffer(it->second);
}

Buffer& BufferManager::open(std::filesystem::path const& path) {
    auto key = path.string();
    auto it = _buffers.find(key);
    if (it != _buffers.end())
        return it->second;
    auto [inserted, _] = _buffers.emplace(key, Buffer{key, path});
    return inserted->second;
}
