#pragma once

#include "buffer.hpp"

#include <string>
#include <vector>

class BufferListBuffer : public Buffer {
public:
    BufferListBuffer(std::vector<std::string> names, std::size_t startLine = 0);

    bool readonly() const override { return true; }
    std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols) const override;

    // Return the buffer name on the line the cursor is currently on.
    std::string selectedName() const;

    void refresh(std::vector<std::string> names, std::size_t startLine = 0);

private:
    std::vector<std::string> _names;
};
