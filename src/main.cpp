#include "buffer/buffer_manager.hpp"
#include "error_log.hpp"
#include "terminal/screen.hpp"
#include "terminal/setup_terminal.hpp"

#include <exception>

int main(int argc, char* argv[]) {
  try {
    SetupTerminal terminal;
    Screen screen;
    BufferManager bufferManager(screen);
    screen.setBufferManager(bufferManager);

    if (argc > 1) {
      for (int i = 1; i < argc; ++i)
        bufferManager.open(argv[i]);
      auto& last = bufferManager.open(argv[argc - 1]);
      screen.setBuffer(last);
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
