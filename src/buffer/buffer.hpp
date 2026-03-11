#pragma once

#include "types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Represents a block of text with an insertion point. The underlying storage
// is a flat std::vector<char>. The point tracks the current position for
// insert and overwrite operations.
class Buffer {
public:
    Buffer() = default;
    explicit Buffer(std::string name);
    Buffer(std::string name, std::filesystem::path const& path);

    void insert(char c);
    void overwrite(char c);

    // Append a rows x cols screen image to `out`, starting from buffer
    // position `from`. Each row is preceded by an ANSI cursor-positioning
    // escape, and a final escape repositions the cursor to the point.
    void render(std::string& out, BufferPoint from, RowIdx rows, ColIdx cols) const;

    BufferPoint point() const { return _point; }
    std::string const& name() const { return _name; }

private:
    std::string _name;
    std::vector<char> _data;
    BufferPoint _point = 0;
    std::optional<std::filesystem::path> _path;
};
