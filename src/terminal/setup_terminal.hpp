#pragma once

#include <termios.h>

// Configures the terminal for raw mode on construction and restores the
// original settings on destruction. Also manages the alternate screen buffer.
class SetupTerminal {
public:
    SetupTerminal();
    ~SetupTerminal();

private:
    struct termios _saved_flags;
};
