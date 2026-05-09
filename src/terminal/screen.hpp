#pragma once

#include "buffer/buffer_sink.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/dir_buffer.hpp"
#include "clipboard.hpp"
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
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

class Buffer;
class BufferManager;
struct MouseEvent;

// Manages terminal I/O and the main input loop.
//
// On construction, queries the terminal size. draw() calls Buffer::render()
// to produce a screen image into an internal buffer, then writes it in a
// single writeAll() syscall. loop() reads raw keystrokes and forwards
// them to the Buffer.
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

  // Move the focused view to the given 1-based line (clamped to the
  // buffer's last line). Used by main() to honour `cx path:LINE`.
  void gotoLine(std::size_t line);

  // Insert `text` into the focused view at the cursor. Used by main()
  // to seed the *stdin* scratch buffer when stdin is piped.
  void insertText(std::string_view text);

private:
  // Centralised action handler: executes the effect of the given Action.
  void dispatchAction(Action action);
  // Try to dispatch an Esc-chord for the given byte. Returns true if
  // the byte was a recognised command, false otherwise.
  bool handleEscChord(char ch);

  // Returns true and sets a status message if the current view is in
  // history mode (i.e. read-only-browse), so the caller can `break`
  // out of the action handler. Returns false otherwise.
  bool requireNotInHistoryMode();

  // draw() helpers. Both append directly to _renderBuf.
  // drawModeline composes the focused-or-non-focused modeline for a
  // panel: cursor row:col + history badge + prompt/status + buffer
  // name and mode suffix, with truncation and right-alignment.
  void drawModeline(Panel& panel, bool focused);
  // placePanelId emits the small panel-id digit in the gutter's
  // top-right cell when there are multiple panels.
  void placePanelId(Panel const& panel, bool multiPanel);
  // Walk buffers looking for the next modified one. If found, switch to
  // it and show a save prompt. Returns true when no modified buffers
  // remain (caller should exit the loop).
  bool tryExit();
  // Kill the named buffer via BufferManager and switch all panels
  // (including non-focused) that show it to the replacement buffer.
  // Handles the focused panel's prevBuf clearing. The caller must
  // clear its own prevBuf before calling if appropriate.
  void killBufferAndSwitchPanels(std::string const& name);

  // Loop sub-handlers. The caller in loop() always redraws after the
  // handler returns; the handlers may issue intermediate draws (e.g.
  // a status update during a chord prompt) but do not signal that
  // back to the caller.
  void handlePromptInput(char c);
  void handleEscapeInput(std::optional<char>& pendingChar);
  // Bare Esc-Esc behaviour: dismiss a readonly overlay if one is
  // active; otherwise clear the mark on the focused view. Shared by
  // both the fast (sequence-of-bytes) and slow (paused-Esc) paths.
  void handleBareEscEsc();
  // Apply a parsed SGR mouse event: focus the panel under the cursor
  // and either move the buffer point (button 0) or scroll the panel
  // (wheel up/down). Other buttons are ignored.
  void handleMouseEvent(MouseEvent const& event);
  void handleCtrlXInput();
  void handleNormalInput(char c, std::optional<char>& pendingChar);
  void resize();
  // Focused panel's content dimensions (rows minus modeline, total cols).
  RowIdx rowsForBuffer() const;
  ColIdx colsForBuffer() const;

  // Update _buf and _view from the focused panel in _layout.
  void updateFocusPointers();

  // Inspect the buffer's detected line ending style. Shows a status
  // message for mixed endings, or prompts the user to translate CRLF
  // or lone CR to Unix line endings. Acknowledges the style on the
  // buffer so it is not flagged again on subsequent switches.
  void checkLineEndings(Buffer& buf);

  // Per-panel overlay buffer accessor (focused panel's prevBuf).
  Buffer*& prevBuf();

  // Prompt construction helpers.
  void openGoToLinePrompt();
  void openSaveAsPrompt();
  void openSwitchBufferPrompt();
  void openFindPrompt(bool backward);
  // Two-step replace flow: prompt for pattern, then replacement, then
  // iterate forward from the cursor with a y/n/!/q confirmation per match.
  void openReplacePrompt();
  // Find the next match for `pattern` starting at the current cursor; on
  // hit, highlight it and open the y/n/!/q confirm prompt. On miss, set a
  // status with the running count.
  void replacePump(std::string pattern, std::string replacement, std::size_t replaced);
  // Confirm prompt for a single replace decision. Stores the matched range
  // so 'y'/'n' know whether to advance past or replace the highlighted hit.
  void openReplaceConfirmPrompt(std::string pattern, std::string replacement, std::size_t replaced,
                                BufferPoint matchStart, BufferPoint matchEnd);
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

  std::string _statusMsg;
  std::optional<std::chrono::steady_clock::time_point> _statusExpiry;
  // Wrap mode and whitespace visibility live per-view on BufferView
  // (see _view->wrap() / _view->showWhitespace()), so different panels
  // can have different settings.
  bool _killPending = false; // kill buffer after SaveAs completes
  bool _exitPending = false; // exit after all modified buffers are handled

  PasteRing _pasteRing;
  Clipboard _clipboard;

  // After every keystroke, mirror the active mark region (if any)
  // into the X11 PRIMARY selection so middle-click paste picks it up.
  // Internal dedup means cursor moves that don't change the selected
  // text cost zero forks.
  void syncPrimarySelection();

  struct PasteState {
    BufferPoint start;
    BufferPoint end;
    std::size_t rbIdx;
  };
  std::optional<PasteState> _pasteState;

  // Find state.
  void findNext();
  void findPrev();
  void blockNext();
  void blockPrev();
  void onFindMatch(std::size_t line, std::size_t matchStart, std::size_t matchEnd, bool wrapped);
  std::string _lastSearch;
  std::optional<std::chrono::steady_clock::time_point> _highlightExpiry;

  // Recenter (Ctrl-l) cycling state. Each consecutive press advances
  // through the slots; any other action resets it so the next Ctrl-l
  // starts at the middle slot again.
  enum class RecenterSlot { Middle, Top, Bottom };
  std::optional<RecenterSlot> _lastRecenter;
};
