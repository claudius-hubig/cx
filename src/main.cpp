#include "buffer/buffer_manager.hpp"
#include "cli.hpp"
#include "error_log.hpp"
#include "fifo.hpp"
#include "file_arg.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"
#include "version.hpp"

#include <fcntl.h>
#include <poll.h>
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
 * @brief Read @p fd to EOF (or to the first read error) into a string.
 *
 * cx uses it for a piped stdin. It puts the data into a scratch buffer
 * before the raw-mode terminal setup takes over fd 0.
 */
static std::string drainFd(int fd) {
  std::string out;
  char buf[4096];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
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

/**
 * @brief The whole contents of @p path if it is a FIFO, else nullopt.
 *
 * Bash's process substitution (`cx <(cmd)`) passes a pipe as /dev/fd/N.
 * cx reads a FIFO argument here, like a piped stdin, and opens it as a
 * scratch buffer (see fifo.hpp). For a named FIFO, this waits for a writer,
 * as `cat` does. The terminal is not set up yet, so Ctrl-C still works.
 * Returns nullopt also if the FIFO cannot be opened, so the normal file
 * path reports the problem.
 */
static std::optional<std::string> readFifo(std::string const& path) {
  if (!isFifo(path))
    return std::nullopt;
  auto reader = FifoReader::open(path);
  if (!reader)
    return std::nullopt;
  pollfd pfd{reader->fd(), POLLIN, 0};
  for (;;) {
    int rc = poll(&pfd, 1, -1);
    if (rc < 0 && errno != EINTR)
      break;
    if (rc > 0 && reader->readAvailable())
      break;
  }
  return reader->takeText();
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
  auto const& [files, theme] = std::get<cli::OpenFiles>(*command);

  // FIFO arguments are read first, before stdin is drained and replaced,
  // so `cmd | cx /dev/stdin` works too.
  std::vector<std::optional<std::string>> fifoTexts;
  for (auto const& arg : files)
    fifoTexts.push_back(readFifo(parseFileArg(arg).first));

  // If stdin is a pipe (no terminal attached), drain it before raw mode
  // takes over. Then dup /dev/tty over fd 0 so SetupTerminal's tcgetattr
  // and the input loop see a real terminal. If /dev/tty cannot be
  // opened, exit cleanly with a message — there is no way to run the
  // editor without a controlling terminal.
  std::string pipedStdin;
  bool stdinPiped = !isatty(STDIN_FILENO);
  if (stdinPiped) {
    pipedStdin = drainFd(STDIN_FILENO);
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
    screen.setTheme(theme);
    BufferManager bufferManager(screen);
    screen.setBufferManager(bufferManager);

    if (!files.empty()) {
      Buffer* last = nullptr;
      // The line number to jump to in the last-opened buffer (if any
      // arg used `path:LINE` syntax — only the final value is honoured).
      std::optional<std::size_t> jumpTo;
      for (std::size_t i = 0; i < files.size(); ++i) {
        auto [path, line] = parseFileArg(files[i]);
        std::error_code ec;
        if (auto const& text = fifoTexts[i]) {
          last = &bufferManager.create("*" + path + "*");
          screen.setBuffer(*last);
          screen.insertText(*text);
          screen.gotoLine(1);
        } else if (std::filesystem::is_directory(path, ec))
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
