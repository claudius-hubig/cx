#include "terminal/setup_terminal.hpp"
#include "terminal/screen.hpp"

int main() {
    try {
        SetupTerminal terminal;
        Screen screen;
        screen.loop();
    } catch (...) {
    }
}
