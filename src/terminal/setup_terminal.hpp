#pragma once

#include <termios.h>

// Configures the terminal for raw mode on construction and restores the
// original settings on destruction. Also manages the alternate screen buffer.
//
// stdin file flags (fcntl F_GETFL) are intentionally NOT saved or
// restored — `cx` relies on blocking reads with SIGWINCH-driven EINTR,
// and the shell's default O_NONBLOCK-cleared state is the contract.
//
// Note: cfmakeraw() is BSD/glibc, not strict POSIX. CLAUDE.md already
// accepts POSIX deviations for terminal I/O; this class is one of them.
class SetupTerminal {
public:
  SetupTerminal();
  ~SetupTerminal();

  SetupTerminal(SetupTerminal const&) = delete;
  SetupTerminal& operator=(SetupTerminal const&) = delete;

private:
  // Initialised by tcgetattr() in the constructor. Value-initialised here
  // so that, if tcgetattr fails and we throw, the destructor running
  // during stack unwind doesn't pass garbage to tcsetattr.
  struct termios _saved_flags{};
};
