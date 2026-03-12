#pragma once

#include "types.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <tuple>
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
    void deleteAtPoint();
    void deleteBeforePoint();
    void moveLeft();
    void moveRight();
    void moveUp();
    void moveDown();

    // Advance _viewFrom to the start of the next line.
    void scrollDown();

    // Compute the screen row of the point relative to _viewFrom,
    // given a screen column width of `cols`.
    std::size_t screenRow(ColIdx cols) const;

    // Append a rows x cols screen image to `out`, starting from _viewFrom.
    // Each row is preceded by an ANSI cursor-positioning escape. Returns
    // the screen (row, col) of the point for the caller to position the
    // terminal cursor.
    std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols) const;

    BufferPoint point() const { return _point; }
    std::string const& name() const { return _name; }

private:
    std::string _name;
    std::vector<char> _data;
    BufferPoint _point = 0;
    BufferPoint _viewFrom = 0;
    std::optional<std::filesystem::path> _path;
};
