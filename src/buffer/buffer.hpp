#pragma once

#include "types.hpp"

#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Abstract base for all buffer types. Provides text storage, a point
// (insertion cursor), navigation, and rendering. Editing operations are
// guarded by readonly() so that derived classes can forbid mutation.
class Buffer {
public:
    virtual ~Buffer() = default;

    Buffer(Buffer const&) = delete;
    Buffer& operator=(Buffer const&) = delete;
    Buffer(Buffer&&) = delete;
    Buffer& operator=(Buffer&&) = delete;

    virtual bool readonly() const = 0;

    // Non-virtual editing wrappers that guard on readonly().
    void insert(char c)       { if (!readonly()) doInsert(c); }
    void deleteAtPoint()      { if (!readonly()) doDeleteAtPoint(); }
    void deleteBeforePoint()  { if (!readonly()) doDeleteBeforePoint(); }
    void overwrite(char c)    { if (!readonly()) doOverwrite(c); }

    void moveLeft();
    void moveRight();
    void moveUp();
    void moveDown();
    void moveHome();
    void moveEnd();

    // Move point to the start of the given 1-based line number and center
    // _viewFrom so the line appears in the middle of the screen.
    void gotoLine(std::size_t line, RowIdx screenRows);

    // Advance _viewFrom to the start of the next line.
    void scrollDown();

    // Move _viewFrom back to the start of the previous line.
    void scrollUp();

    // Compute the screen row of the point relative to _viewFrom,
    // given a screen column width of `cols`.
    std::size_t screenRow(ColIdx cols) const;

    // Append a rows x cols screen image to `out`, starting from _viewFrom.
    // Each row is preceded by an ANSI cursor-positioning escape. Returns
    // the screen (row, col) of the point for the caller to position the
    // terminal cursor.
    virtual std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols) const;

    BufferPoint point() const { return _point; }
    BufferPoint viewFrom() const { return _viewFrom; }
    std::string const& name() const { return _name; }

    // Return the 1-indexed line and column of the point.
    std::tuple<std::size_t, std::size_t> lineCol() const;

protected:
    explicit Buffer(std::string name);

    void doInsert(char c);
    void doDeleteAtPoint();
    void doDeleteBeforePoint();
    void doOverwrite(char c);

    std::string _name;
    std::vector<char> _data;
    BufferPoint _point = 0;
    BufferPoint _viewFrom = 0;
    std::optional<ColIdx> _desiredCol;
};
