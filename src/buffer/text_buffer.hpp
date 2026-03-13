#pragma once

#include "buffer.hpp"

#include <filesystem>
#include <optional>

class TextBuffer : public Buffer {
public:
    explicit TextBuffer(std::string name);
    TextBuffer(std::string name, std::filesystem::path const& path);

    bool readonly() const override { return false; }

private:
    std::optional<std::filesystem::path> _path;
};
