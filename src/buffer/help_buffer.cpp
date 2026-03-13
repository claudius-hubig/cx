#include "help_buffer.hpp"

#include <string_view>

static constexpr auto kHelpText =
    "cx - Text Editor Help\n"
    "\n"
    "Key Bindings:\n"
    "  Ctrl-C        Exit\n"
    "  Ctrl-O        Open file\n"
    "  Ctrl-L        Go to line\n"
    "  Ctrl-H        Show this help\n"
    "  Ctrl-X        Insert \"hello\"\n"
    "\n"
    "  Arrow keys    Move cursor\n"
    "  Home          Start of line\n"
    "  End           End of line\n"
    "  Backspace     Delete before cursor\n"
    "  Delete        Delete at cursor\n"
    "  Enter         Insert newline\n"
    "\n"
    "Press 'q' or Esc to return to editing.\n";

HelpBuffer::HelpBuffer() : Buffer("*help*") {
    _data.assign(std::string_view{kHelpText}.begin(),
                 std::string_view{kHelpText}.end());
}
