#pragma once

#include "types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <tuple>
#include <variant>

class Buffer;
class BufferManager;

enum class Action {
    Exit,
    GoToLine,
    Help,
    Save,
    BufferList,
    SwitchBuffer,
    ToggleWhitespace,
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

    // Display a timed status message on the status line.
    void setStatus(std::string msg);

private:
    // Interpret a control character and return the appropriate response:
    // Action::Exit on Ctrl-C, a string to insert on Ctrl-X, or monostate
    // for unhandled inputs.
    std::variant<std::monostate, std::string, Action> handleSpecialInput(char c);
    void resize();
    // Number of rows available for buffer content (total minus status line).
    RowIdx bufRows() const;
    std::tuple<RowIdx, ColIdx> _size;
    Buffer* _buf = nullptr;
    BufferManager* _bufMgr = nullptr;
    std::string _out;
    enum class Prompt { None, GoToLine, SaveAs, SwitchBuffer, ConfirmSave };
    Prompt _prompt = Prompt::None;
    std::string _prompt_input;
    Buffer* _prevBuf = nullptr;
    std::string _statusMsg;
    std::optional<std::chrono::steady_clock::time_point> _statusExpiry;
    bool _showWhitespace = false;
};
