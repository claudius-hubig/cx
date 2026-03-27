#include "buffer/buffer_manager.hpp"
#include "error_log.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"

#include <exception>
#include <filesystem>

int main(int argc, char* argv[]) {
  try {
    SetupTerminal terminal;
    Screen screen;
    BufferManager bufferManager(screen);
    screen.setBufferManager(bufferManager);

    if (argc > 1) {
      Buffer* last = nullptr;
      for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        if (std::filesystem::is_directory(argv[i], ec))
          last = &bufferManager.openDir(argv[i]);
        else
          last = &bufferManager.open(argv[i]);
      }
      screen.setBuffer(*last);
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
