#include "setup_terminal.hpp"
#include "error_log.hpp"

#include <unistd.h>

SetupTerminal::SetupTerminal() {
    if (tcgetattr(STDIN_FILENO, &_saved_flags) != 0)
        logError("tcgetattr failed");
    struct termios raw = _saved_flags;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        logError("tcsetattr failed (raw mode)");

    // Switch to the alternate screen buffer and move the cursor to the
    // top-left. The previous screen contents are saved and restored on exit.
    if (write(STDOUT_FILENO, "\x1b[?1049h\x1b[H\x1b[?25h\x1b[2 q", 23) == -1)
        logError("write failed (alt screen setup)");
}

SetupTerminal::~SetupTerminal() {
    if (write(STDOUT_FILENO, "\x1b[?1049l", 8) == -1)
        logError("write failed (alt screen teardown)");
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_flags) != 0)
        logError("tcsetattr failed (restore)");
}
