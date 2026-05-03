#include "setup_terminal.hpp"
#include "error_log.hpp"

#include <unistd.h>

#include <stdexcept>
#include <string_view>

static constexpr std::string_view kAltScreenOn = "\x1b[?1049h\x1b[H\x1b[?25h\x1b[2 q";
static constexpr std::string_view kAltScreenOff = "\x1b[?1049l";

SetupTerminal::SetupTerminal() {
  // If tcgetattr fails we have no usable termios to base raw mode on.
  // Throw so main() exits cleanly without ever entering raw mode or
  // touching the alternate screen — which means the destructor must
  // not run, hence the throw before _haveSavedFlags conceptually
  // becomes true. _saved_flags stays value-initialised regardless.
  if (tcgetattr(STDIN_FILENO, &_saved_flags) != 0) {
    logError("tcgetattr failed");
    throw std::runtime_error("tcgetattr failed (cannot enter raw mode)");
  }
  struct termios raw = _saved_flags;
  cfmakeraw(&raw);
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    logError("tcsetattr failed (raw mode)");
    throw std::runtime_error("tcsetattr failed (cannot enter raw mode)");
  }

  // Switch to the alternate screen buffer and move the cursor to the
  // top-left. The previous screen contents are saved and restored on exit.
  if (write(STDOUT_FILENO, kAltScreenOn.data(), kAltScreenOn.size()) == -1)
    logError("write failed (alt screen setup)");
}

SetupTerminal::~SetupTerminal() {
  if (write(STDOUT_FILENO, kAltScreenOff.data(), kAltScreenOff.size()) == -1)
    logError("write failed (alt screen teardown)");
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_flags) != 0)
    logError("tcsetattr failed (restore)");
}
