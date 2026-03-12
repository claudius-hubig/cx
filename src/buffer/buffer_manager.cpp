#include "buffer_manager.hpp"
#include "terminal/screen.hpp"

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

BufferManager::BufferManager(Screen& screen)
    : _helpBuffer("*help*"), _screen(screen) {
    for (char c : std::string_view{kHelpText})
        _helpBuffer.insert(c);
    _helpBuffer.setReadonly(true);

    auto [it, _] = _buffers.emplace("*1*", Buffer{"*1*"});
    _screen.setBuffer(it->second);
}

Buffer& BufferManager::open(std::filesystem::path const& path) {
    auto key = path.string();
    auto it = _buffers.find(key);
    if (it != _buffers.end())
        return it->second;
    auto [inserted, _] = _buffers.emplace(key, Buffer{key, path});
    return inserted->second;
}
