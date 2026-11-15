#pragma once

/**
 * @file screen.hpp
 * @brief Screen — the terminal I/O hub: the main input loop, action
 *        dispatch, drawing, prompts, find/replace, mouse handling, and
 *        subprocess execution.
 *
 * Screen owns the active buffer pointer and the panel layout, and
 * implements BufferSink so BufferManager can push buffer switches to it.
 * Session-wide state that belongs to no single buffer (active prompt, find
 * state, paste ring, clipboard adapter, status message) lives here; per-view
 * display toggles live on BufferView.
 */

#include "bookmarks.hpp"
#include "buffer/buffer_sink.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/dir_buffer.hpp"
#include "buffer/grep_buffer.hpp"
#include "clipboard.hpp"
#include "key_sequence.hpp"
#include "panel_layout.hpp"
#include "paste_ring.hpp"
#include "prompt.hpp"
#include "shortcut.hpp"
#include "types.hpp"

#include <chrono>
#include <filesystem>
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

/** @brief How much stdout Screen::runPipeCommand() collects before it stops the command. */
struct PipeLimit {
  std::size_t lines = static_cast<std::size_t>(-1); /**< '\\n'-terminated lines. */
  std::size_t bytes = static_cast<std::size_t>(-1); /**< Bytes; must be > 0. */
};

/**
 * @brief Manages terminal I/O and the main input loop.
 *
 * On construction, queries the terminal size. draw() calls
 * Buffer::render() to produce a screen image into an internal buffer, then
 * writes it in a single writeAll() syscall. loop() reads raw keystrokes
 * and dispatches them.
 */
class Screen : public BufferSink {
public:
  Screen();

  /** @brief BufferSink hook: make @p buf active and redraw. */
  void setBuffer(Buffer& buf) override;
  /** @brief Wire up the BufferManager used for buffer switching/killing. */
  void setBufferManager(BufferManager& mgr);

  /**
   * @brief Render the buffer to the terminal and place the hardware cursor
   *        at the current point.
   */
  void draw();

  /** @brief How loop() ended. */
  enum class Exit {
    Normal, /**< The exit command: every modified buffer has been dealt with. */
    /** Nobody is left to ask about the modified buffers: stdin was
     *  closed, or SIGTERM came again with the exit flow unanswered.
     *  emergencySave() is due. */
    Emergency,
  };

  /**
   * @brief Run the main input loop.
   *
   * Reads keystrokes one character at a time, dispatches them, and redraws
   * after each. Returns on the exit command or when stdin is closed.
   *
   * SIGTERM is taken as the exit command (Esc-c), whatever the editor is
   * waiting for: the modified buffers are asked about, one by one. A
   * second SIGTERM while that is still going on ends the loop with
   * Exit::Emergency, and so does stdin being closed (the terminal went
   * away): the caller is to restore the terminal and call
   * emergencySave(), which leaves the unsaved buffers in copies.
   */
  [[nodiscard]] Exit loop();

  /**
   * @brief Write every modified buffer that has a file to a copy next to
   *        that file, without asking, and write one line per buffer to
   *        stdout saying what became of it. Modified buffers without a
   *        file are abandoned (and listed).
   *
   * The copy is a new file `<file>.unsaved-<UTC time>`, readable by its
   * owner only. The file itself is never written: nobody was asked, and
   * the buffer may be damaged, so the user decides afterwards which of
   * the two to keep.
   *
   * The way out when nobody can be asked any more: on a second SIGTERM
   * (see loop()), and when the editor crashes or SIGHUP, SIGINT or
   * SIGQUIT ends it (see FatalSignalHook), where it runs in a signal
   * handler on whatever state the editor was in.
   *
   * @pre The terminal has been restored, so the lines stay on the user's
   *      screen.
   * @return False if a buffer could not be written.
   */
  bool emergencySave() noexcept;

  /** @brief Display a timed status message on the status line. */
  void setStatus(std::string msg);

  /**
   * @brief Move the focused view to the 1-based @p line (clamped to the
   *        last line).
   *
   * Used by main() to honour `cx path:LINE`.
   */
  void gotoLine(std::size_t line);

  /**
   * @brief Insert @p text into the focused view at the cursor.
   *
   * Used by main() to seed the *stdin* scratch buffer when stdin is piped.
   */
  void insertText(std::string_view text);

private:
  /** @brief Centralised action handler: execute the effect of @p action. */
  void dispatchAction(Action action);
  /**
   * @brief Try to dispatch an Esc-chord for @p ch.
   * @return True if the byte was a recognised command.
   */
  bool handleEscChord(char ch);

  /**
   * @brief Guard for actions that are invalid while browsing history.
   * @return True (after posting a status message) when the current view is
   *         in history mode, so the caller can `break` out of the handler.
   */
  bool requireNotInHistoryMode();

  /**
   * @brief Compose a panel's modeline (cursor row:col, history badge,
   *        prompt/status, buffer name + mode) into @c _renderBuf.
   * @return The screen column of the prompt cursor cell when @p focused
   *         and a prompt is active, unless the panel is too narrow to
   *         show it; the cursor row is the modeline row.
   */
  std::optional<std::size_t> drawModeline(Panel& panel, bool focused);
  /**
   * @brief Emit the small panel-id digit in the gutter's top-right cell
   *        when there are multiple panels.
   */
  void placePanelId(Panel const& panel, bool multiPanel);
  /**
   * @brief Walk buffers looking for the next modified one, switching to it
   *        and showing a save prompt.
   * @return True when no modified buffers remain (the caller should exit).
   */
  bool tryExit();
  /**
   * @brief Act on one SIGTERM: the first starts the exit flow, as Esc-c
   *        does, after cancelling a prompt that is in its way (an exit
   *        flow that is already open is left as it is).
   * @return True for a further SIGTERM while that flow is still open:
   *         nobody is answering, so the caller is to save and leave.
   */
  bool handleTerminationRequest();
  /** @brief Close the active prompt, if any, as a bare Esc does. */
  void abortPrompt();
  /** @brief Put every buffer whose history is being browsed back at its newest version. */
  void leaveHistoryModeInAllPanels();
  /**
   * @brief Kill the named buffer and switch every panel showing it to the
   *        replacement buffer.
   *
   * Handles the focused panel's prevBuf clearing; the caller must clear its
   * own prevBuf beforehand if appropriate.
   */
  void killBufferAndSwitchPanels(std::string const& name);

  /**
   * @name Input-loop sub-handlers
   *
   * loop() always redraws after a handler returns; handlers may issue
   * intermediate draws but do not signal that back to the caller.
   */
  /** @{ */
  void handlePromptInput(char c);
  /**
   * @brief Handle a cursor or editing key typed into the active prompt:
   *        history, cursor movement, and forward deletion.
   */
  void handlePromptKey(KeyPress key);
  /**
   * @brief Apply a shortcut-table action to the active prompt's input.
   *
   * Handles the mark, cut/copy/paste/paste-cycle (through the shared
   * paste ring and X11 CLIPBOARD) and chunk deletion; other actions are
   * ignored while a prompt is open.
   * @param action The action the key resolved to in the shortcut table.
   * @param lastPaste The paste made by the previous key, if any, which a
   *        PasteCycle replaces.
   */
  void handlePromptAction(Action action, std::optional<Prompt::Paste> lastPaste);
  /** @brief Push X11 CLIPBOARD onto the paste ring if it differs from the top entry. */
  void pullExternalClipboard();
  void handleEscapeInput(std::optional<char>& pendingChar);
  /**
   * @brief Apply a cursor or editing key (arrows, Home/End, PgUp/PgDn,
   *        Insert, Delete, with their modifiers) to the focused view.
   */
  void handleKeyPress(KeyPress key);
  /**
   * @brief Bare Esc-Esc: dismiss a read-only overlay if active, otherwise
   *        clear the mark on the focused view.
   *
   * Shared by the fast (sequence-of-bytes) and slow (paused-Esc) paths.
   */
  void handleBareEscEsc();
  /**
   * @brief Apply a parsed SGR mouse event.
   *
   * Focuses the panel under the cursor, then: left-press moves the point
   * (and arms a drag), left-drag extends a mark→point selection,
   * middle-press pastes the X11 PRIMARY selection, and the wheel scrolls
   * the panel.
   */
  void handleMouseEvent(MouseEvent const& event);
  /**
   * @brief Continue an in-progress left-drag.
   *
   * Clamps (row,col) into the focused panel's content, autoscrolls at the
   * top/bottom content row, sets the mark to the drag anchor (overriding
   * any prior mark), and moves the point so the mark→point region tracks
   * the drag. syncPrimarySelection() then mirrors it to X11 PRIMARY.
   */
  void extendDragSelection(std::size_t row, std::size_t col);
  void handleCtrlXInput();
  void handleNormalInput(char c, std::optional<char>& pendingChar);
  /** @} */
  void resize();
  /** @brief Focused panel's content height (rows minus modeline). */
  RowIdx rowsForBuffer() const;
  /** @brief Focused panel's total column count. */
  ColIdx colsForBuffer() const;

  /** @brief Rebind @c _buf / @c _view to the focused panel in @c _layout. */
  void updateFocusPointers();

  /**
   * @brief Common tail of a buffer switch, after the focused panel already
   *        shows @p buf: rebind focus pointers, reset paste state, touch MRU,
   *        check line endings, redraw.
   */
  void finishBufferSwitch(Buffer& buf);

  /**
   * @brief Inspect the buffer's detected line-ending style.
   *
   * Shows a status message for mixed endings, or prompts the user to
   * translate CRLF / lone CR to Unix endings. Acknowledges the style on the
   * buffer so it is not flagged again on subsequent switches.
   */
  void checkLineEndings(Buffer& buf);

  /** @brief Accessor for the focused panel's overlay-restore slot. */
  Buffer*& prevBuf();

  /**
   * @name Prompt construction helpers
   */
  /** @{ */
  void openGoToLinePrompt();
  void openSaveAsPrompt();
  void openSwitchBufferPrompt();
  void openFindPrompt(bool backward);
  /** @brief Prompt for a pattern, then run a find-in-files search. */
  void openGrepPrompt();
  /**
   * @brief Two-step replace flow: prompt for pattern, then replacement,
   *        then iterate forward with a y/n/!/q confirmation per match.
   */
  void openReplacePrompt();
  /**
   * @brief Find the next match for @p pattern from the cursor.
   *
   * On a hit, highlights it and opens the y/n/!/q confirm prompt; on a
   * miss, sets a status with the running count.
   */
  void replacePump(std::string pattern, std::string replacement, std::size_t replaced);
  /**
   * @brief Confirm prompt for a single replace decision.
   *
   * Stores the matched range so 'y'/'n' know whether to advance past or
   * replace the highlighted hit.
   */
  void openReplaceConfirmPrompt(std::string pattern, std::string replacement, std::size_t replaced,
                                BufferPoint matchStart, BufferPoint matchEnd);
  void openSetModePrompt();
  /** @brief C-x b: prompt for a slot and bookmark the current position. */
  void openSetBookmarkPrompt();
  /** @brief C-x j: prompt for a slot and jump to its bookmark. */
  void openJumpBookmarkPrompt();
  /**
   * @brief Jump to the bookmark stored in @p slot.
   *
   * Switches the focused panel's buffer if the bookmark points elsewhere,
   * then sets the point, clamped to the target buffer's current contents.
   * Drops the slot if its buffer no longer exists.
   */
  void jumpToBookmark(char slot);
  /**
   * @brief Rename the focused buffer via BufferManager and re-key any
   *        bookmarks pointing at it under the new name.
   *
   * Wraps every Save-As call site so none can rename a buffer without
   * keeping its bookmarks in sync.
   * @return false if the rename was refused (the target path is already
   *         open in another buffer); the status line says so and any
   *         pending kill/exit is cancelled.
   */
  bool renameBufferAndRekeyBookmarks(std::string oldName, std::string const& newName);
  /**
   * @brief Esc-x: prompt for an action by description, run it, then surface
   *        the action's primary shortcut as a status hint.
   */
  void openExecuteCommandPrompt();
  /**
   * @brief Esc-X: regex find.
   *
   * Prompts for an ECMAScript pattern, highlights the next match, and
   * leaves @c _lastRegex set so a subsequent Esc-X with empty input reuses
   * it.
   */
  void openRegexFindPrompt();
  /**
   * @brief Esc-Y: regex find-and-replace.
   *
   * Two-step prompt (pattern, then replacement template with $0..$9 / $&
   * back-references), then walks forward through matches with the same
   * y/n/!/q decisions as plain Replace.
   */
  void openRegexReplacePrompt();
  void regexReplacePump(std::string pattern, std::string replacement, std::size_t replaced);
  void openRegexReplaceConfirmPrompt(std::string pattern, std::string replacement,
                                     std::size_t replaced, BufferPoint matchStart,
                                     BufferPoint matchEnd);
  /** @brief Forward regex search using @c _lastRegex; mirrors findNext(). */
  void regexFindNext();
  void openConfirmPrompt(std::string question, std::function<void(std::string const&)> onResult,
                         std::function<void()> onAbort);
  /** @brief Confirm-kill prompt shared by KillBuffer and tryExit. */
  void openConfirmKillPrompt(std::string question);
  /** @brief Save the current buffer and, if @c _killPending, kill it afterwards. */
  void saveAndMaybeKill();
  /** @} */

  /**
   * @brief Run the prompt's autocomplete and cache the result.
   *
   * If autocomplete returns an early-exit result, closes the prompt and
   * calls onResult.
   * @return True if the prompt was closed by early exit.
   */
  bool runAutocomplete();

  /**
   * @brief Working directory for the current buffer.
   *
   * The buffer's parent directory if it has a path, else the process
   * working directory. Used by all subprocess callers.
   */
  std::string bufferWorkDir() const;

  /**
   * @brief Directory a find-in-files search starts from.
   *
   * The browsed directory when the current buffer is a DirBuffer (which
   * has no path of its own, so bufferWorkDir() would answer with the
   * process cwd), else the current file's parent, else the process cwd.
   */
  std::filesystem::path searchStartDir() const;

  /**
   * @brief Run a find-in-files search for @p pattern and show the results.
   *
   * Uses `git grep` from the repository root when the start directory is
   * inside a work tree, else `grep -r` from the start directory itself.
   */
  void runGrep(std::string const& pattern);

  /**
   * @brief Open the file behind a grep result at its matching line.
   *
   * Shows it in the other panel, splitting vertically first when only one
   * panel is open, so the result listing stays visible while you walk it.
   */
  void openGrepHit(GrepHit const& hit);

  /** @brief Result of a runPipeCommand() invocation. */
  struct PipeResult {
    std::string stdoutData;
    std::string stderrData;
    int exitCode;
    bool killed;            /**< The user killed the command at the kill prompt. */
    bool truncated = false; /**< Stopped at the PipeLimit; exitCode is then -1. */
  };
  /**
   * @brief Run a subprocess with a kill-prompt poll loop.
   *
   * Used by Format, Tab-format, PipeReplace and Grep. Forks/execs @p args,
   * pipes @p input to stdin, and monitors the child with poll(); after 10s
   * without output it offers a kill prompt. Once stdout reaches @p limit
   * the command is killed (SIGKILL, so only for commands that need no
   * cleanup) and stdoutData is cut after the last whole line within it.
   * @return The captured output, or nullopt on fork/pipe failure.
   */
  std::optional<PipeResult> runPipeCommand(std::vector<std::string> const& args,
                                           std::string_view input, std::string const& workDir,
                                           PipeLimit limit = {});

  /** @brief Format the current buffer (or @p lineRange) via clang-format. */
  void executeFormat(std::optional<std::pair<std::size_t, std::size_t>> lineRange);

  /** @brief Pipe-replace: prompt for a shell command. */
  void openPipeReplacePrompt();
  /** @brief Pipe the region through @p command and replace it with the output. */
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
  bool _killPending = false; /**< Kill the buffer after SaveAs completes. */
  bool _exitPending = false; /**< Exit after all modified buffers are handled. */
  /** A SIGTERM has asked for the exit flow that is open (_exitPending). */
  bool _exitBySignal = false;
  /**
   * @brief Runtime mouse-tracking toggle.
   *
   * SetupTerminal enables ?1000h / ?1006h on construction; this mirrors
   * that default so the toggle starts in agreement with the terminal.
   */
  bool _mouseEnabled = true;

  /**
   * @brief Buffer point where the left button went down.
   *
   * Set on left-press, cleared on release. While set, left-motion events
   * extend a selection anchored here; nullopt means no drag in progress.
   */
  std::optional<BufferPoint> _dragAnchor;

  PasteRing _pasteRing;
  Clipboard _clipboard;
  Bookmarks _bookmarks;

  /**
   * @brief Mirror the active mark region into the X11 PRIMARY selection.
   *
   * Called after every keystroke so middle-click paste picks it up.
   * Internal dedup means cursor moves that don't change the selected text
   * cost zero forks.
   */
  void syncPrimarySelection();

  /** @brief In-flight paste-cycle state (Esc-y after a paste). */
  struct PasteState {
    BufferPoint start;
    BufferPoint end;
    std::size_t rbIdx;
  };
  std::optional<PasteState> _pasteState;

  /**
   * @name Find state
   */
  /** @{ */
  void findNext();
  void findPrev();
  void blockNext();
  void blockPrev();
  void onFindMatch(std::size_t line, std::size_t matchStart, std::size_t matchEnd, bool wrapped);
  std::string _lastSearch;
  /**
   * @brief Last regex pattern (raw source; recompiled per search).
   *
   * Separate from @c _lastSearch so Ctrl-f and Esc-X do not clobber each
   * other's history.
   */
  std::string _lastRegex;
  std::optional<std::chrono::steady_clock::time_point> _highlightExpiry;
  /** @} */

  /** @brief Recenter (Ctrl-l) target slot. */
  enum class RecenterSlot { Middle, Top, Bottom };
  /**
   * @brief Recenter cycling state: each consecutive Ctrl-l advances through
   *        the slots; any other action resets it to Middle.
   */
  std::optional<RecenterSlot> _lastRecenter;
};
