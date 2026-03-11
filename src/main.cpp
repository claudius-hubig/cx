#include "terminal/setup_terminal.hpp"
#include "terminal/screen.hpp"
#include "buffer/buffer_manager.hpp"

int main() {
    try {
        SetupTerminal terminal;
        Screen screen;
        BufferManager bufferManager(screen);
        screen.setBufferManager(bufferManager);
        screen.loop();
    } catch (...) {
    }
}
