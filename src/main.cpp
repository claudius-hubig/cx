#include "buffer/buffer_manager.hpp"
#include "cli.hpp"
#include "error_log.hpp"
#include "file_arg.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"
#include "version.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/**
 * @brief Drain all bytes from fd 0 into a string.
 *
 * Used when stdin is a pipe so the data can be seeded into a scratch
 * buffer before raw-mode terminal setup takes over fd 0.
 */
static std::string drainStdin() {
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n > 0)
      out.append(buf, static_cast<std::size_t>(n));
    else if (n == 0)
      break;
    else if (errno == EINTR)
      continue;
    else
      break;
  }
  return out;
}

int main(int argc, char* argv[]) {
  // Options first: --help / --version must not touch stdin or the terminal.
  auto command = cli::parse(std::vector<std::string_view>(argv + 1, argv + argc));
  if (!command) {
    std::fprintf(stderr, "cx: %s\nTry 'cx --help' for more information.\n",
                 command.error().c_str());
    return 2;
  }
  if (std::holds_alternative<cli::ShowHelp>(*command)) {
    std::fputs(cli::usage(argc > 0 ? argv[0] : "cx").c_str(), stdout);
    return 0;
  }
  if (std::holds_alternative<cli::ShowVersion>(*command)) {
    std::fputs(versionText().c_str(), stdout);
    return 0;
  }
  auto const& files = std::get<cli::OpenFiles>(*command).args;

  // If stdin is a pipe (no terminal attached), drain it before raw mode
  // takes over. Then dup /dev/tty over fd 0 so SetupTerminal's tcgetattr
  // and the input loop see a real terminal. If /dev/tty cannot be
  // opened, exit cleanly with a message — there is no way to run the
  // editor without a controlling terminal.
  std::string pipedStdin;
  bool stdinPiped = !isatty(STDIN_FILENO);
  if (stdinPiped) {
    pipedStdin = drainStdin();
    int tty = open("/dev/tty", O_RDWR);
    if (tty < 0) {
      std::fprintf(stderr, "cx: cannot open /dev/tty: %s\n", std::strerror(errno));
      return 1;
    }
    if (dup2(tty, STDIN_FILENO) < 0) {
      std::fprintf(stderr, "cx: dup2 /dev/tty failed: %s\n", std::strerror(errno));
      return 1;
    }
    close(tty);
  }

  try {
    SetupTerminal terminal;
    Screen screen;
    BufferManager bufferManager(screen);
    screen.setBufferManager(bufferManager);

    if (!files.empty()) {
      Buffer* last = nullptr;
      // The line number to jump to in the last-opened buffer (if any
      // arg used `path:LINE` syntax — only the final value is honoured).
      std::optional<std::size_t> jumpTo;
      for (auto const& arg : files) {
        auto [path, line] = parseFileArg(arg);
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec))
          last = &bufferManager.openDir(path);
        else
          last = &bufferManager.open(path);
        jumpTo = line;
      }
      screen.setBuffer(*last);
      if (jumpTo)
        screen.gotoLine(*jumpTo);
      screen.setStatus("Opened " + std::to_string(files.size()) +
                       (files.size() == 1 ? " file" : " files"));
      screen.draw();
    } else if (stdinPiped && !pipedStdin.empty()) {
      Buffer& buf = bufferManager.create("*stdin*");
      screen.setBuffer(buf);
      screen.insertText(pipedStdin);
      screen.gotoLine(1);
      screen.setStatus("Read " + std::to_string(pipedStdin.size()) + " byte(s) from stdin");
      screen.draw();
    }

    // Should the editor crash or be told to quit by a signal, it saves
    // what it can on the way out.
    FatalSignalHook saveOnSignal([](void* s) noexcept { static_cast<Screen*>(s)->emergencySave(); },
                                 &screen);

    if (screen.loop() == Screen::Exit::Emergency) {
      // Nothing may end the save halfway: once the terminal is restored a
      // Ctrl-C raises SIGINT again. The editor exits right after, so the
      // signals held back here are simply dropped.
      sigset_t quit;
      sigemptyset(&quit);
      for (int signo : {SIGTERM, SIGHUP, SIGINT, SIGQUIT})
        sigaddset(&quit, signo);
      sigprocmask(SIG_BLOCK, &quit, nullptr);
      // The list of what was saved belongs on the user's normal screen.
      terminal.restore();
      return screen.emergencySave() ? 0 : 1;
    }
  } catch (std::exception const& e) {
    logError(e.what());
    return 1;
  } catch (...) {
    logError("unknown exception");
    return 1;
  }
}
