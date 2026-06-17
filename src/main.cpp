#include "buffer/buffer_manager.hpp"
#include "error_log.hpp"
#include "file_arg.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>

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

    if (argc > 1) {
      Buffer* last = nullptr;
      // The line number to jump to in the last-opened buffer (if any
      // arg used `path:LINE` syntax — only the final value is honoured).
      std::optional<std::size_t> jumpTo;
      for (int i = 1; i < argc; ++i) {
        auto [path, line] = parseFileArg(argv[i]);
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
      screen.setStatus("Opened " + std::to_string(argc - 1) + (argc - 1 == 1 ? " file" : " files"));
      screen.draw();
    } else if (stdinPiped && !pipedStdin.empty()) {
      Buffer& buf = bufferManager.create("*stdin*");
      screen.setBuffer(buf);
      screen.insertText(pipedStdin);
      screen.gotoLine(1);
      screen.setStatus("Read " + std::to_string(pipedStdin.size()) + " byte(s) from stdin");
      screen.draw();
    }

    screen.loop();
  } catch (std::exception const& e) {
    logError(e.what());
    return 1;
  } catch (...) {
    logError("unknown exception");
    return 1;
  }
}
