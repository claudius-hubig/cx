#include "setup_terminal.hpp"
#include "error_log.hpp"

#include <signal.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>

// 1049: alternate screen buffer; 1000: button press/release tracking;
// 1002: button-event tracking (motion reports *only while a button is
// held*) so left-drag selection works; 1006: SGR extended coordinates
// so col/row > 223 can be reported. We deliberately stop at 1002 and do
// NOT enable any-event tracking (1003): idle pointer moves with no
// button down generate no input, keeping the byte volume low.
// 2004: bracketed paste — pasted text arrives between ESC[200~ and
// ESC[201~ rather than as keystrokes (see key_sequence.hpp).
static constexpr std::string_view kAltScreenOn =
    "\x1b[?1049h\x1b[H\x1b[?25h\x1b[2 q\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?2004h";
static constexpr std::string_view kAltScreenOff =
    "\x1b[?2004l\x1b[0 q\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?1049l";

namespace {

// The signals that end the process by default and can reach an editor:
// kill, a closed terminal window, and Ctrl-C / Ctrl-\ sent from outside
// (raw mode keeps the keyboard from generating them); then the crashes,
// the ones a bug ends it with: a bad memory access, abort() (where an
// uncaught exception or a failed assertion ends up), and the rarer faults
// of the same kind.
constexpr std::array kFatalSignals = {SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV,
                                      SIGABRT, SIGBUS, SIGFPE, SIGILL};

// termios state for the signal handler. Written once, before the handlers
// are installed, so the handler only ever reads it.
struct termios g_savedFlags;
// The dispositions the handlers replaced, put back by restore().
std::array<struct sigaction, kFatalSignals.size()> g_savedActions;

// The handler's own stack. A stack overflow (deep recursion, which is how
// std::regex fails on a long line) arrives as SIGSEGV with no room left
// on the stack it overflowed, so the handler could not run there. A fixed
// size, as SIGSTKSZ is not a constant: the handler needs very little, the
// hook it calls (which writes files) a good deal more.
std::array<char, 256 * 1024> g_handlerStack;
// The alternate stack that was set before, put back by restore().
stack_t g_savedStack;

// The FatalSignalHook in effect. The hook is the one the handler looks
// at, so it is set last and cleared first.
std::atomic<FatalSignalHook::Fn> g_hook = nullptr;
void* g_hookContext = nullptr;
// How long the hook gets before SIGALRM ends the process.
constexpr unsigned kHookSeconds = 60;

// Put the terminal back, run the hook, then let the signal end the
// process as it would have: the handler was installed with SA_RESETHAND,
// so the default action is in place again and the re-raised signal is
// delivered when this returns, with a core dump where the signal has one.
//
// Apart from the hook only async-signal-safe calls are made (write,
// tcsetattr, alarm, raise). The hook is taken out before it is called, so
// it runs once: a fault inside it, or another of these signals arriving
// meanwhile, comes back here, finds no hook and ends the process.
void restoreTerminalAndReraise(int signo) {
  if (write(STDOUT_FILENO, kAltScreenOff.data(), kAltScreenOff.size()) == -1) {
    // Nothing useful can be done about it from a signal handler.
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_savedFlags);
  if (auto hook = g_hook.exchange(nullptr)) {
    alarm(kHookSeconds);
    hook(g_hookContext);
  }
  raise(signo);
}

} // namespace

FatalSignalHook::FatalSignalHook(Fn hook, void* context) {
  g_hookContext = context;
  g_hook = hook;
}

FatalSignalHook::~FatalSignalHook() { g_hook = nullptr; }

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

  // A signal that ends the process skips the destructor, which would
  // leave the shell in raw mode on the alternate screen with mouse
  // reporting on. Should the alternate stack be refused, the handler
  // runs on the normal one, which only a stack overflow lacks.
  g_savedFlags = _saved_flags;
  stack_t stack = {};
  stack.ss_sp = g_handlerStack.data();
  stack.ss_size = g_handlerStack.size();
  sigaltstack(&stack, &g_savedStack);
  struct sigaction sa = {};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = restoreTerminalAndReraise;
  sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
  for (std::size_t i = 0; i < kFatalSignals.size(); ++i)
    sigaction(kFatalSignals[i], &sa, &g_savedActions[i]);
}

SetupTerminal::~SetupTerminal() { restore(); }

void SetupTerminal::restore() {
  if (std::exchange(_restored, true))
    return;
  for (std::size_t i = 0; i < kFatalSignals.size(); ++i)
    sigaction(kFatalSignals[i], &g_savedActions[i], nullptr);
  sigaltstack(&g_savedStack, nullptr);
  if (write(STDOUT_FILENO, kAltScreenOff.data(), kAltScreenOff.size()) == -1)
    logError("write failed (alt screen teardown)");
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &_saved_flags) != 0)
    logError("tcsetattr failed (restore)");
}
