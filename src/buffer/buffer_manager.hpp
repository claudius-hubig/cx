#pragma once

#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/text_buffer.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Screen;

class BufferManager {
public:
    explicit BufferManager(Screen& screen);

    // Open (or return existing) buffer for the given path. Reads the file
    // contents into a new TextBuffer if not already open.
    Buffer& open(std::filesystem::path const& path);

    Buffer& helpBuffer() { return *_helpBuffer; }
    BufferListBuffer& bufferListBuffer();

    // Find a buffer by name. Returns nullptr if not found.
    Buffer* find(std::string const& name);

    // Create a new empty buffer with the given name. Returns the new buffer.
    Buffer& create(std::string name);

    // Return the names of all buffers (excluding help and buffer list),
    // sorted alphabetically.
    std::vector<std::string> bufferNames() const;

    // Mark a buffer as most recently used.
    void touch(std::string const& name);

    // Return the name of the second most recently used buffer, or empty.
    std::string secondMRU() const;

private:
    std::unique_ptr<HelpBuffer> _helpBuffer;
    std::unique_ptr<BufferListBuffer> _bufferListBuffer;
    std::unordered_map<std::string, std::unique_ptr<TextBuffer>> _buffers;
    std::vector<std::string> _mru; // most recently used first
    Screen& _screen;
};
