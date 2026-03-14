#pragma once

#include "buffer.hpp"

class HelpBuffer : public Buffer {
public:
    HelpBuffer();

    bool readonly() const override { return true; }
    std::size_t screenRow(ColIdx cols) const override;
    std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                     bool showWhitespace = false) const override;
};
