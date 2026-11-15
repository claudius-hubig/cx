#pragma once

/**
 * @file setup_terminal.hpp
 * @brief SetupTerminal — RAII raw-mode and alternate-screen configuration.
 */

#include <termios.h>

/**
 * @brief Configures the terminal for raw mode and the alternate screen on
 *        construction, restoring the original state on destruction.
 *
 * While an instance exists, SIGTERM, SIGHUP, SIGINT and SIGQUIT restore
 * the terminal too before ending the process as they would have: the
 * destructor does not run for them. Nothing else is done on a signal, so
 * unsaved buffers are lost as before. At most one instance may exist at a
 * time (the handler's state is per process).
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

private:
  /**
   * @brief termios state captured by tcgetattr() in the constructor.
   *
   * Value-initialised so that if tcgetattr fails and the constructor
   * throws, the destructor running during stack unwind does not pass
   * garbage to tcsetattr.
   */
  struct termios _saved_flags{};
};
