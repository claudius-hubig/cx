#include "error_log.hpp"
#include "terminal/setup_terminal.hpp"
#include "terminal/screen.hpp"
#include "buffer/buffer_manager.hpp"

#include <exception>

int main() {
    try {
        SetupTerminal terminal;
        Screen screen;
        BufferManager bufferManager(screen);
        screen.setBufferManager(bufferManager);
        screen.loop();
    } catch (std::exception const& e) {
        logError(e.what());
    } catch (...) {
        logError("unknown exception");
    }
}
