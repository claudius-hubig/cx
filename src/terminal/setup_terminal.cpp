#include "setup_terminal.hpp"
#include "error_log.hpp"

#include <unistd.h>

#include <stdexcept>
#include <string_view>

// 1049: alternate screen buffer; 1000: button press/release tracking;
// 1002: button-event tracking (motion reports *only while a button is
// held*) so left-drag selection works; 1006: SGR extended coordinates
// so col/row > 223 can be reported. We deliberately stop at 1002 and do
// NOT enable any-event tracking (1003): idle pointer moves with no
// button down generate no input, keeping the byte volume low.
static constexpr std::string_view kAltScreenOn =
    "\x1b[?1049h\x1b[H\x1b[?25h\x1b[2 q\x1b[?1000h\x1b[?1002h\x1b[?1006h";
static constexpr std::string_view kAltScreenOff =
    "\x1b[0 q\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?1049l";

SetupTerminal::SetupTerminal() {
  // Throw before touching raw mode or the alternate screen so a failure
  // here leaves the terminal untouched and the destructor's teardown a no-op.
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

  if (write(STDOUT_FILENO, kAltScreenOn.data(), kAltScreenOn.size()) == -1)
    logError("write failed (alt screen setup)");
}

SetupTerminal::~SetupTerminal() {
  if (write(STDOUT_FILENO, kAltScreenOff.data(), kAltScreenOff.size()) == -1)
    logError("write failed (alt screen teardown)");
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_flags) != 0)
    logError("tcsetattr failed (restore)");
}
