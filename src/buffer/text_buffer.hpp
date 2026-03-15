#pragma once

#include "buffer.hpp"

#include <filesystem>
#include <optional>
#include <string>

class TextBuffer : public Buffer {
public:
    explicit TextBuffer(std::string name);
    TextBuffer(std::string name, std::filesystem::path const& path);

    bool readonly() const override { return false; }

    bool hasPath() const override { return _path.has_value(); }
    void setPath(std::filesystem::path p);

    // Write _data to _path. Returns true on success.
    bool save() override;

private:
    std::optional<std::filesystem::path> _path;
};
