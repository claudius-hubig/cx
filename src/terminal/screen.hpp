#pragma once

#include "buffer/buffer_sink.hpp"
#include "types.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

class Buffer;
class BufferManager;

enum class Action {
  Exit,
  GoToLine,
  Help,
  Save,
  KillBuffer,
  BufferList,
  SwitchBuffer,
  ToggleWhitespace,
  ToggleWrap,
};

// Manages terminal I/O and the main input loop.
//
// On construction, queries the terminal size. draw() calls Buffer::render()
// to produce a screen image and writes it row-by-row to the terminal.
// loop() reads raw keystrokes and forwards them to the Buffer.
class Screen : public BufferSink {
public:
  Screen();

  void setBuffer(Buffer &buf) override;
  void setBufferManager(BufferManager &mgr);

  // Render the buffer to the terminal and move the hardware cursor
  // to the current buffer point position.
  void draw();

  // Read keystrokes one character at a time, forward them to the buffer,
  // and redraw after each keystroke. Returns when stdin is closed or on
  // Action::Exit.
  void loop();

  // Display a timed status message on the status line.
  void setStatus(std::string msg);

private:
  // Interpret a control character and return the appropriate response:
  // Action::Exit on Ctrl-C, a string to insert on Ctrl-X, or monostate
  // for unhandled inputs.
  std::variant<std::monostate, std::string, Action> handleSpecialInput(char c);
  // Try to dispatch an Esc-chord for the given byte. Returns true if
  // the byte was a recognised command, false otherwise.
  bool handleEscChord(char ch);
  void resize();
  // Number of rows available for buffer content (total minus status line).
  RowIdx bufRows() const;
  std::tuple<RowIdx, ColIdx> _size;
  Buffer *_buf = nullptr;
  BufferManager *_bufMgr = nullptr;
  std::string _renderBuf;
  enum class Prompt {
    None,
    GoToLine,
    SaveAs,
    SwitchBuffer,
    ConfirmSave,
    ConfirmKill
  };
  Prompt _prompt = Prompt::None;
  std::string _prompt_input;
  Buffer *_prevBuf = nullptr;
  std::string _statusMsg;
  std::optional<std::chrono::steady_clock::time_point> _statusExpiry;
  bool _showWhitespace = false;
  bool _wrap = false;
  bool _killPending = false; // kill buffer after SaveAs completes

  // Auto-completion state for SwitchBuffer prompt.
  void updateCompletions();
  std::vector<std::pair<std::string, std::string>>
      _completions;        // {displayName, bufName}
  int _completionIdx = -1; // -1 = no selection, 0+ = cycling
};
