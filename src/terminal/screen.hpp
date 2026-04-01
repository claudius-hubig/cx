#pragma once

#include "buffer/buffer_sink.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/dir_buffer.hpp"
#include "panel_layout.hpp"
#include "paste_ring.hpp"
#include "prompt.hpp"
#include "shortcut.hpp"
#include "types.hpp"

#include <chrono>
#include <functional>
#include <memory>
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
  bool handlePromptInput(char c);
  bool handleEscapeInput(std::optional<char>& pendingChar);
  bool handleNormalInput(char c, std::optional<char>& pendingChar);
  void resize();
  // Number of rows available for buffer content (total minus status line).
  RowIdx rowsForBuffer() const;

  // Update _buf and _view from the focused panel in _layout.
  void updateFocusPointers();

  // Prompt construction helpers.
  void openGoToLinePrompt();
  void openSaveAsPrompt();
  void openSwitchBufferPrompt();
  void openFindPrompt(bool backward);
  void openSetModePrompt();
  void openConfirmPrompt(std::string question, std::function<void(std::string const&)> onResult,
                         std::function<void()> onAbort);
  // Confirm-kill prompt shared by KillBuffer and tryExit.
  void openConfirmKillPrompt(std::string question);
  // Save the current buffer and, if _killPending, kill it afterwards.
  void saveAndMaybeKill();

  // Run the prompt's autocomplete and cache the result. If autocomplete
  // returns an early-exit result, closes the prompt and calls onResult.
  // Returns true if the prompt was closed by early exit.
  bool runAutocomplete();

  // Working directory for the current buffer: parent dir if it has a path,
  // else the process working directory. Used by all subprocess callers.
  std::string bufferWorkDir() const;

  // Subprocess execution with a kill-prompt poll loop. Used by Format,
  // Tab-format, and PipeReplace. Returns nullopt on fork/pipe failure.
  struct PipeResult {
    std::string stdoutData;
    std::string stderrData;
    int exitCode;
    bool killed;
  };
  std::optional<PipeResult> runPipeCommand(std::vector<std::string> const& args,
                                           std::string_view input, std::string const& workDir);

  // Format the current buffer using clang-format via runPipeCommand.
  void executeFormat(std::optional<std::pair<std::size_t, std::size_t>> lineRange);

  // Pipe-replace: prompt for a shell command, pipe the region through it.
  void openPipeReplacePrompt();
  void executePipeReplace(std::string command);

  std::tuple<RowIdx, ColIdx> _size;
  std::unique_ptr<PanelLayout> _layout;
  Buffer* _buf = nullptr;
  BufferView* _view = nullptr;
  BufferManager* _bufMgr = nullptr;
  std::string _renderBuf;

  std::optional<Prompt> _prompt;
  PromptHistory _promptHistory;

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
};
