#pragma once

#include "buffer.hpp"

class HelpBuffer : public Buffer {
public:
    HelpBuffer();

    bool readonly() const override { return true; }
};
