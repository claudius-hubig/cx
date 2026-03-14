#pragma once

#include <cstddef>

struct RowIdx {
    explicit RowIdx(std::size_t value) : value(value) {}
    std::size_t value;
};

struct ColIdx {
    explicit ColIdx(std::size_t value) : value(value) {}
    std::size_t value;
};

using BufferPoint = std::size_t;
