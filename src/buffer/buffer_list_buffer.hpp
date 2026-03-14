#pragma once

#include "buffer.hpp"

#include <string>
#include <vector>

class BufferListBuffer : public Buffer {
public:
    BufferListBuffer(std::vector<std::string> names, std::size_t startLine = 0);

    bool readonly() const override { return true; }
    std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                     bool showWhitespace = false) const override;

    // Return the buffer name on the line the cursor is currently on.
    std::string selectedName() const;

    void refresh(std::vector<std::string> names, std::size_t startLine = 0);

private:
    // Count newlines before pos in _data.
    std::size_t lineAt(std::size_t pos) const;
    std::vector<std::string> _names;
};
