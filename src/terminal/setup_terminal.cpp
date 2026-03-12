#include "setup_terminal.hpp"

#include <unistd.h>

SetupTerminal::SetupTerminal() {
    tcgetattr(STDIN_FILENO, &_saved_flags);
    struct termios raw = _saved_flags;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    // Switch to the alternate screen buffer and move the cursor to the
    // top-left. The previous screen contents are saved and restored on exit.
    write(STDOUT_FILENO, "\x1b[?1049h\x1b[H\x1b[?25h\x1b[2 q", 23);
}

SetupTerminal::~SetupTerminal() {
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_flags);
}
