#pragma once

#include "buffer/buffer.hpp"

#include <filesystem>
#include <string>
#include <unordered_map>

class Screen;

class BufferManager {
public:
    explicit BufferManager(Screen& screen);

    // Open (or return existing) buffer for the given path. Reads the file
    // contents into a new Buffer if not already open.
    Buffer& open(std::filesystem::path const& path);

private:
    std::unordered_map<std::string, Buffer> _buffers;
    Screen& _screen;
};
