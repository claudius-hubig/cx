#pragma once

/**
 * @file setup_terminal.hpp
 * @brief SetupTerminal — RAII raw-mode and alternate-screen configuration,
 *        and FatalSignalHook, the last thing run when a signal ends
 *        the editor.
 */

#include <termios.h>

/**
 * @brief Configures the terminal for raw mode and the alternate screen on
 *        construction, restoring the original state on destruction.
 *
 * While an instance exists, the signals that end the process restore the
 * terminal too before ending it as they would have, since the destructor
 * does not run for them: SIGTERM (until Screen takes it over), SIGHUP,
 * SIGINT and SIGQUIT, and the crashes SIGSEGV (a stack overflow included),
 * SIGABRT, SIGBUS, SIGFPE and SIGILL. Between the two the
 * FatalSignalHook runs, if there is one; nothing else is done on a
 * signal, so without a hook unsaved buffers are lost. At most one
 * instance may exist at a time (the handler's state is per process).
 *
 * stdin file flags (fcntl F_GETFL) are intentionally NOT saved or restored
 * — cx relies on blocking reads with SIGWINCH-driven EINTR, and the
 * shell's default O_NONBLOCK-cleared state is the contract.
 *
 * Note: cfmakeraw() is BSD/glibc, not strict POSIX; CLAUDE.md already
 * accepts POSIX deviations for terminal I/O, and this class is one of them.
 */
class SetupTerminal {
public:
  SetupTerminal();
  ~SetupTerminal();

  SetupTerminal(SetupTerminal const&) = delete;
  SetupTerminal& operator=(SetupTerminal const&) = delete;

  /**
   * @brief Restore the terminal now rather than on destruction, for
   *        output that is to stay on the user's normal screen. Does
   *        nothing the second time.
   */
  void restore();

private:
  /**
   * @brief termios state captured by tcgetattr() in the constructor.
   *
   * Value-initialised so that if tcgetattr fails and the constructor
   * throws, the destructor running during stack unwind does not pass
   * garbage to tcsetattr.
   */
  struct termios _saved_flags{};
  bool _restored = false; /**< restore() has run. */
};

/**
 * @brief While an instance exists, a signal that ends the process under a
 *        SetupTerminal (see there for the list) calls `hook(context)`
 *        once, after the terminal has been restored and before the signal
 *        takes effect.
 *
 * The hook runs inside a signal handler, on the handler's stack: after a
 * fault of unknown cause, or in the middle of whatever the editor was
 * doing when the signal arrived. So nothing it does is safe in the strict
 * sense: the data it reads may be damaged or half updated, and a lock it
 * needs (the allocator's) may be held by the interrupted code. It is the
 * last chance to save the user's work, so it runs anyway, and what it
 * writes should not replace anything the user still has. A fault inside
 * it, or a second such signal, ends the process, and so does a hook that
 * has not returned after a minute.
 *
 * @p context must outlive the instance. At most one instance may exist at
 * a time.
 */
class FatalSignalHook {
public:
  using Fn = void (*)(void* context) noexcept;

  FatalSignalHook(Fn hook, void* context);
  ~FatalSignalHook();

  FatalSignalHook(FatalSignalHook const&) = delete;
  FatalSignalHook& operator=(FatalSignalHook const&) = delete;
};
