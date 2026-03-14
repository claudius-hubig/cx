#pragma once

#include "buffer.hpp"

#include <string>
#include <vector>

class BufferListBuffer : public Buffer {
public:
    BufferListBuffer(std::vector<std::string> names, std::string const& startName = {});

    bool readonly() const override { return true; }
    std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                     bool showWhitespace = false) const override;

    // Return the buffer name on the line the cursor is currently on.
    std::string selectedName() const;

    void refresh(std::vector<std::string> names, std::string const& startName = {});

private:
    // Count newlines before pos in _data.
    std::size_t lineAt(std::size_t pos) const;

    // Display text for each line (with indentation).
    std::vector<std::string> _displayLines;
    // Buffer name for each display line (empty for directory headers).
    std::vector<std::string> _lineToName;
};
