#include "buffer/buffer_manager.hpp"
#include "error_log.hpp"
#include "file_arg.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>

int main(int argc, char* argv[]) {
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
