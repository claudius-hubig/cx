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
