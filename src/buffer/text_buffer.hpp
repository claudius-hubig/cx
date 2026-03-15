#pragma once

#include "buffer.hpp"

#include <filesystem>
#include <optional>
#include <string>

class TextBuffer : public Buffer {
public:
    /// Construct a buffer with the given name and no backing file.
    /// The buffer starts empty and unmodified.
    explicit TextBuffer(std::string name);

    /// Construct a buffer that reads its initial content from path.
    /// If the file is missing or unreadable the buffer is empty but
    /// hasPath() still returns true so save() can write to it.
    TextBuffer(std::string name, std::filesystem::path const& path);

    bool readonly() const override { return false; }

    bool hasPath() const override { return _path.has_value(); }

    /// Set the backing file path. Updates _name to the path string.
    /// Must coordinate with BufferManager::rename() to keep the map
    /// and MRU in sync.
    void setPath(std::filesystem::path p);

    /// Write _data to _path as a truncating binary write. Returns false
    /// if no path is set or the write fails. Clears _modified on success.
    bool save() override;

private:
    std::optional<std::filesystem::path> _path;
};
