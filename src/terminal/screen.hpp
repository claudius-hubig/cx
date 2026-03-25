#pragma once

#include "buffer/buffer_sink.hpp"
#include "paste_ring.hpp"
#include "shortcut.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

class Buffer;
class BufferManager;

// Manages terminal I/O and the main input loop.
//
// On construction, queries the terminal size. draw() calls Buffer::render()
// to produce a screen image and writes it row-by-row to the terminal.
// loop() reads raw keystrokes and forwards them to the Buffer.
class Screen : public BufferSink {
public:
  Screen();

  void setBuffer(Buffer& buf) override;
  void setBufferManager(BufferManager& mgr);

  // Render the buffer to the terminal and move the hardware cursor
  // to the current buffer point position.
  void draw();

  // Read keystrokes one character at a time, forward them to the buffer,
  // and redraw after each keystroke. Returns when stdin is closed or on
  // Esc-c exit.
  void loop();

  // Display a timed status message on the status line.
  void setStatus(std::string msg);

private:
  // Centralised action handler: executes the effect of the given Action.
  void dispatchAction(Action action);
  // Try to dispatch an Esc-chord for the given byte. Returns true if
  // the byte was a recognised command, false otherwise.
  bool handleEscChord(char ch);
  // Walk buffers looking for the next modified one. If found, switch to
  // it and show a save prompt. Returns true when no modified buffers
  // remain (caller should exit the loop).
  bool tryExit();

  // Loop sub-handlers. Each returns true if it already called draw()
  // (so the caller should skip its trailing draw()).
  // Unified Y/N/Esc confirmation handler. Dispatches to the given
  // callbacks on Y/y, N/n, or Esc respectively. Returns true if the
  // handler already called draw().
  bool handleConfirm(char c, std::function<bool()> onAccept, std::function<bool()> onDecline,
                     std::function<bool()> onAbort);
  bool handleTextPrompt(char c);
  void submitSwitchBuffer();
  void submitSaveAs();
  void submitFind();
  void submitGoToLine();
  void submitSetMode();
  bool handleEscapeInput(std::optional<char>& pendingChar);
  bool handleNormalInput(char c, std::optional<char>& pendingChar);
  void resize();
  // Number of rows available for buffer content (total minus status line).
  RowIdx rowsForBuffer() const;
  std::tuple<RowIdx, ColIdx> _size;
  Buffer* _buf = nullptr;
  BufferManager* _bufMgr = nullptr;
  std::string _renderBuf;
  enum class Prompt {
    None,
    GoToLine,
    SaveAs,
    SwitchBuffer,
    ConfirmSave,
    ConfirmKill,
    ConfirmRevert,
    Find,
    SetMode
  };
  Prompt _prompt = Prompt::None;
  std::string _prompt_input;
  Buffer* _prevBuf = nullptr;
  std::string _statusMsg;
  std::optional<std::chrono::steady_clock::time_point> _statusExpiry;
  bool _showWhitespace = false;
  bool _wrap = false;
  bool _killPending = false; // kill buffer after SaveAs completes
  bool _exitPending = false; // exit after all modified buffers are handled

  PasteRing _pasteRing;
  struct PasteState {
    BufferPoint start;
    BufferPoint end;
    std::size_t rbIdx;
  };
  std::optional<PasteState> _pasteState;

  // Find state.
  void findNext();
  void findPrev();
  void onFindMatch(std::size_t line, std::size_t matchStart, std::size_t matchEnd, bool wrapped);
  std::string _lastSearch;
  std::optional<std::chrono::steady_clock::time_point> _highlightExpiry;

  // Auto-completion state for SwitchBuffer prompt.
  void updateCompletions();
  std::vector<std::pair<std::string, std::string>> _completions; // {displayName, bufName}
  std::optional<std::size_t> _completionIdx;
};
