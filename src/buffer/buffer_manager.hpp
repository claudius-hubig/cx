#pragma once

#include "buffer/buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/text_buffer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

class Screen;

class BufferManager {
public:
    explicit BufferManager(Screen& screen);

    // Open (or return existing) buffer for the given path. Reads the file
    // contents into a new TextBuffer if not already open.
    Buffer& open(std::filesystem::path const& path);

    Buffer& helpBuffer() { return *_helpBuffer; }

private:
    std::unique_ptr<HelpBuffer> _helpBuffer;
    std::unordered_map<std::string, std::unique_ptr<TextBuffer>> _buffers;
    Screen& _screen;
};
