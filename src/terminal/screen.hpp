#pragma once

#include "types.hpp"

#include <string>
#include <tuple>
#include <variant>

class Buffer;
class BufferManager;

enum class Action {
    Exit,
    OpenFile,
};

// Manages terminal I/O and the main input loop.
//
// On construction, queries the terminal size. draw() calls Buffer::render()
// to produce a screen image and writes it row-by-row to the terminal.
// loop() reads raw keystrokes and forwards them to the Buffer.
class Screen {
public:
    Screen();

    void setBuffer(Buffer& buf);
    void setBufferManager(BufferManager& mgr);

    // Render the buffer to the terminal and move the hardware cursor
    // to the current buffer point position.
    void draw();

    // Read keystrokes one character at a time, forward them to the buffer,
    // and redraw after each keystroke. Returns when stdin is closed or on Action::Exit.
    void loop();

    // Interpret a control character and return the appropriate response:
    // Action::Exit on Ctrl-C, a string to insert on Ctrl-X, or monostate
    // for unhandled inputs.
    std::variant<std::monostate, std::string, Action> handleSpecialInput(char c);

private:
    void resize();
    std::tuple<RowIdx, ColIdx> _size;
    Buffer* _buf = nullptr;
    BufferManager* _bufMgr = nullptr;
    BufferPoint _view_from = 0;
    std::string _out;
    bool _prompt_active = false;
    std::string _prompt_input;
};
