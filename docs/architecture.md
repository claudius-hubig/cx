# cx architecture

## Overview

`cx` is a simple terminal text editor. It has two layers:

- The terminal layer handles raw I/O and rendering.
- The buffer layer owns all text content and editing state.

The `BufferSink` interface decouples the two layers, so tests can exercise the buffer layer without a real
terminal. A separate shortcut module is the single source of truth for all keybindings.

```
main()
  └── SetupTerminal   (RAII: raw mode, alternate screen)
  └── Screen          (terminal I/O, key dispatch, drawing)
        ├── Shortcut table  (binding lookup, formatting)
        └── BufferManager   (buffer lifecycle, MRU, completions)
              └── Buffer*   (text, cursor, viewport, rendering)
                    └── Syntax tokenizers  (per-mode highlighting)
```

---

## Classes

### `main` (`src/main.cpp`)

This is the entry point. Before it touches stdin or the terminal, it parses the command line with `cli::parse()`
(`src/cli.hpp`) into a `cli::Command`: `ShowHelp`, `ShowVersion` or `OpenFiles`. For the first two, it prints
`cli::usage()` or `versionText()` (`src/version.hpp/.cpp`) and exits.

Otherwise, `main` does these steps:

1. It constructs `SetupTerminal`, `Screen` and `BufferManager`, in that order.
2. It hands `OpenFiles::theme` (from `--theme=light|dark`) to `Screen::setTheme()`.
3. It opens the files given on the command line. `parseFileArg()` (`src/file_arg.hpp`) parses each path argument,
   to honour an optional trailing `:LINE` suffix (e.g. `cx src/foo.cpp:42`).
4. It calls `Screen::loop()` to run the editor.

A `FatalSignalHook` calls `Screen::emergencySave()` on a crash, and on a signal that ends the editor. When the loop
ends with `Exit::Emergency`, `main` calls `emergencySave()` itself, after `SetupTerminal::restore()`. It exits with
status 1 if it could not write a buffer.

If stdin is a pipe, `main` reads all of it before the raw-mode setup. It then dups `/dev/tty` over fd 0, so
terminal setup still works, and puts the captured bytes into a `*stdin*` scratch buffer.

A FIFO argument (bash's `cx <(cmd)` passes a pipe as `/dev/fd/N`) has no size and can be read only once. So
`readFifo()` reads it whole with a `FifoReader` (`src/fifo.hpp`), before stdin is drained. `main` opens it as a
scratch buffer named `*<path>*`. There is no file to revert from or save to. For a named FIFO, the read waits for a
writer with no time limit, as `cat` does. The terminal is not set up yet, so Ctrl-C still works.
`BufferManager::refreshDisplayNames()` shows a `*scratch*` name whole, so a `/` in it does not shorten it to a
filename.

---

### `SetupTerminal` (`src/terminal/setup_terminal.hpp/.cpp`)

This is an RAII wrapper around the terminal configuration. On construction it does these steps:

- It saves the current `termios` state and puts stdin into raw mode.
- It switches the terminal to the alternate screen buffer.
- It turns on SGR mouse tracking: `?1000h` for press and release, `?1002h` for motion while a button is held (for
  drag-select), and `?1006h` for extended coordinates. It deliberately does not turn on `?1003h` (any motion).
- It turns on bracketed paste (`?2004h`).

On destruction, it turns off mouse tracking and bracketed paste, restores the saved state and returns to the normal
screen. `restore()` does the same earlier, for output that must stay on the normal screen. Cleanup happens even on
an exception.

A signal that ends the process would skip the destructor. So while an instance exists, a handler first puts the
terminal back and then raises the signal again with its default action (core dump included). This covers:

- SIGTERM (until `Screen` takes it over), SIGHUP, SIGINT and SIGQUIT
- the crashes SIGSEGV, SIGABRT, SIGBUS, SIGFPE and SIGILL

A signal that was ignored on entry stays ignored. Examples are the SIGHUP of nohup, and SIGINT and SIGQUIT for a
command that a script starts in the background. The handler runs on an alternate stack (`sigaltstack`), because a
stack overflow leaves no room for it on the normal stack.

`FatalSignalHook` (same files) registers what happens between restoring the terminal and dying, for each of those
signals. `main` uses it to save the modified buffers. The hook runs in the signal handler, after a fault or in the
middle of whatever the editor was doing. So it is best effort by nature, and it must not replace anything that the
user still has. These rules apply:

- The hook runs at most once. A fault inside it ends the process.
- SIGALRM ends a hook that has not returned after 10 seconds. For example, the hook can wait on an allocator lock
  that the interrupted code holds.
- The other fatal signals are blocked while the hook runs. With the terminal restored, a Ctrl-C raises SIGINT
  again, and that must not cut the save short. Afterwards the blocked signals are discarded, so the process still
  ends with the signal that started it.

`main` blocks SIGTERM, SIGHUP, SIGINT and SIGQUIT around its own emergency save, for the same reason. SIGHUP,
SIGINT and SIGQUIT go through this handler and not through the input loop, so that they also end an editor that
does not respond.

---

### `Shortcut` (`src/shortcut.hpp/.cpp`)

This module defines:

- the `Action` enum, with one value per editor command
- the `Shortcut` struct (modifier flags and a key)
- a single `constexpr` binding table

The table is the **only** place where keybindings are defined. Both input dispatch and the help display read from
it.

Key API:

- `handleShortcut(Shortcut)` is the forward lookup. It returns the `Action` for a shortcut, or `nullopt` if it does
  not know the shortcut.
- `shortcutForAction(Action)` is the reverse lookup. It returns the primary shortcut for an action, which is the
  first match in the table.
- `formatShortcut(Shortcut)` returns a readable label (e.g. `"Ctrl-s"`, `"Esc-?"`, `"Ctrl-Bksp"`).
- `descriptionForAction(Action)` returns a short description.
- `shortcutFromCtrlChar(char)` and `shortcutFromEscChar(char)` convert raw terminal bytes to `Shortcut` values.

Actions: `Help`, `Exit`, `GoToLine`, `Save`, `KillBuffer`, `DeleteLine`, `SwitchBuffer`, `ToggleMark`, `Cut`,
`Copy`, `Paste`, `PasteCycle`, `DeleteChunkBefore`, `Revert`, `VersionHistory`, `Find`, `FindBackward`, `Replace`,
`BufferList`, `PasteRing`, `ToggleWhitespace`, `ToggleWrap`, `Format`, `ToggleComment`, `SetMode`, `DirBrowser`,
`PipeReplace`, `SplitVertical`, `SplitHorizontal`, `ClosePanel`, `FocusPanel1` to `FocusPanel6`, `FocusMRUPanel`,
`NextBlock`, `PrevBlock`, `Recenter`, `ToggleCurrentLine`, `ExecuteCommand`, `Reflow`, `RegexFind`,
`RegexReplace`, `ToggleLineNumbers`, `ToggleMouseTracking`, `SetBookmark`, `JumpBookmark`,
`SelectAllOccurrences`, `AddCursorDown`, `AddCursorUp`.

Some actions have two shortcuts. For example, both `Ctrl-Bksp` (which arrives as `Ctrl-h`) and `Esc-Bksp` reach
`DeleteChunkBefore`. The table has the bare-Ctrl path first, so help and reverse-lookup callers see that one.

A few actions have no shortcut in the table (an empty `Shortcut`), because the input loop drives them from CSI
sequences and not from a chord. These are `ToggleOverwriteMode` (the Insert key) and `AddCursorDown` /
`AddCursorUp` (`Ctrl-Shift-Down/Up`, `ESC[1;6B` / `ESC[1;6A`). They still have table rows, so help lists them and
`Esc-x` can run them by name. Help renders them with `formatRawLine`, because `formatShortcut` has no chord to
format.

---

### `Screen` (`src/terminal/screen.hpp/.cpp`)

`Screen` manages all terminal I/O. It owns the main event loop and the active `Buffer*` pointer. It implements
`BufferSink`, so that `BufferManager` can push buffer switches to it.

#### Drawing

`draw()` calls `Buffer::render()` to produce an ANSI escape string. It then writes that string to stdout, together
with a status line and the cursor position. It also manages timed status messages and the expiry of the find
highlight.

Each frame sets the cursor shape from the overwrite flag of the focused buffer, through DECSCUSR. Insert mode gets
a steady block (`ansi::CursorBlock`), and overwrite mode a steady bar (`ansi::CursorBar`). The shape goes back to
the terminal default when the alternate screen is torn down.

The terminal cursor is hidden in two cases:

- while a prompt is open, because the prompt draws its own cursor
- while `selectsRows()` of the focused buffer is true, because list buffers show the cursor as a reverse-video row

#### Input loop

`loop()` reads raw keystrokes, handles SIGWINCH and SIGTERM, and dispatches to sub-handlers. It waits in
`pselect()`, and blocks the two signals only while it checks their flags. So a signal that arrives just before the
wait does not stay unhandled until the next key.

While more input is already waiting, `loop()` skips the redraw after a key, for at most `kMaxDrawDelay` (30 ms).
This happens with text pasted without bracketed paste, or with a held key. So cx draws a burst a few times, and not
once per byte.

#### Termination

SIGTERM is the exit command:

- The first SIGTERM (`handleTerminationRequest()`) cancels an open prompt and starts the exit flow of Esc-c, which
  asks about each modified buffer. If the user already opened an exit flow, it stays as it is.
- A further SIGTERM while that flow is open means that nobody answers. `loop()` returns `Exit::Emergency`, and
  `main` calls `emergencySave()`.
- A cancelled exit flow resets this, so the next SIGTERM asks again.

The handler only counts requests. The loop acts on them. Some waits could keep the editor from getting back to the
loop, so they give way to a pending request:

- `readByte()`, because the rest of an escape sequence or a paste may never come
- the Esc and C-x prefixes
- `runPipeCommand()`, which stops the running command, as `y` at the kill prompt does

#### Emergency save

`emergencySave()` is the way out when nobody can be asked. cx uses it:

- after that second SIGTERM
- when stdin is closed. `loop()` also returns `Exit::Emergency` when a `read()` returns 0 bytes, because then the
  terminal went away, with or without a SIGHUP.
- from the `FatalSignalHook`, when the editor crashes or when SIGHUP, SIGINT or SIGQUIT ends it

It never writes a buffer's own file. Nobody agreed to that, and in a signal handler the buffer may be damaged or
half updated. Instead, `Buffer::writeCopy()` writes every modified buffer that has a file to a new file next to it:
`<file>.unsaved-<UTC time>`. The copy is owner-only and never replaces a file. The file itself stays as it was last
saved. Modified buffers without a file are abandoned. The restored terminal gets one line per buffer:
`cx: unsaved <file> written to <copy>`, `cx: could not save …` or `cx: discarded …`.

#### Input dispatch

If a `std::exception` escapes a handler, cx logs it and shows it in the status line. It does not end the session.
The handlers are:

- `handlePromptInput` is the generic handler for the active `Prompt`. It drives text entry, history navigation,
  autocomplete and confirmation. Cursor and editing keys go to `handlePromptKey`.
- `handleEscapeInput` handles key escape sequences and Esc-prefix chords. It routes SGR mouse payloads to
  `handleMouseEvent`.
- `handleKeyPress` handles the cursor and editing keys in normal mode: arrows, Home/End, PgUp/PgDn, Insert and
  Delete, with Ctrl or Ctrl-Shift.
- `handleNormalInput` handles printable characters and control keys in normal editing mode.
- `handleMouseEvent` / `extendDragSelection` handle the mouse:
  - A left press positions the cursor and arms a drag. A plain click keeps any existing mark.
  - Left motion sets the mark to the drag anchor (lazily) and extends the `mark`→point region. It scrolls
    automatically at the top and bottom content rows.
  - A left release ends the drag.
  - A middle press pastes the X11 PRIMARY selection at the cursor.
  - The wheel scrolls the panel under the pointer.

Both input paths read a CSI (`ESC [ … final`) or SS3 (`ESC O final`) sequence to its end, and decode it with
`key_sequence.hpp`. So cx drops a key that it has no use for (function keys, …) whole, and its tail bytes never
leak into the buffer or the prompt. cx tells `ESC O` apart from Alt-O by whether a byte follows within the Esc
disambiguation window.

cx reads a bracketed paste (`ESC[200~ … ESC[201~`) whole and inserts it with one `BufferView::insertText()`. That is
one undo step, with no auto-indent or Tab formatting. A prompt takes the first line of the paste. A read-only buffer
or a confirm prompt drops it. If the end marker of a paste gets lost, the paste ends after `kPasteIdleMs` (2 s)
without input, with a status message. Otherwise cx would take every later key as paste text.

#### Action dispatch

`dispatchAction(Action)` is the central switch that carries out every `Action`. Three input paths feed it, all
through `handleShortcut()`:

- the bare Ctrl-key path (`shortcutFromCtrlChar`)
- the Esc-chord path (`shortcutFromEscChar`)
- the two-key C-x prefix path (`shortcutFromCxChar`, driven by `handleCtrlXInput()`)

An unknown shortcut produces a status message. Many actions start with a guard, `requireNotInHistoryMode()`. When a
view is browsing history, it posts a status message and returns `true`, so the caller can `break` out without
effect.

#### Find

`findNext()` and `findPrev()` search forward and backward with smart matching. The search ignores case when the
pattern is lowercase. It skips digits and special characters in the buffer unless they appear in the pattern. On a
match, it puts the cursor after the last matched character and applies a timed highlight.

#### Subprocess execution

`runPipeCommand()` is the one subprocess runner. Format (Esc-f), Tab-format (Cpp mode) and PipeReplace (Ctrl-p) use
it. It forks and execs an external command, connects stdin, stdout and stderr through pipes, and monitors the child
with `ppoll()`.

- The child runs in a process group of its own, so stopping it also stops what it started. (With
  `sh -c 'sleep 60'`, `sleep` is a child of the shell, and it held the output pipe open after the shell was
  killed.)
- After 10 seconds without activity, it shows a kill prompt. `y` sends SIGTERM and then SIGKILL to the process
  group, and `n` dismisses the prompt. A SIGTERM to the editor does the same as `y`. After a kill, it drains what is
  left in the pipes without blocking.
- It handles SIGWINCH during the poll loop, so the terminal redraws correctly while a command runs.
- A pipe that reaches EOF leaves the poll set. Once every pipe is closed, the loop polls for the child's exit and
  does not block in `waitpid()`. So a command that closes its outputs and keeps running (`cmd 2>/dev/null`) neither
  spins the editor nor escapes the kill prompt.
- An optional `PipeLimit` (lines and bytes of stdout) stops a command early. The command is killed, stdout is cut
  after the last whole line within the limit, and the result is marked `truncated`. Grep uses 100,000 lines /
  64 MB.

#### Opening a path from Ctrl-o

`openPath()` opens a path that the user typed into Ctrl-o. A directory opens in the directory browser. For a file or
a FIFO, it first shows "Opening file <path>..." or "Opening fifo <path>..." and draws, so the user knows what a slow
open waits for.

`readFifo()` reads a FIFO whole into a scratch buffer named `*<path>*`. Opening the same FIFO again replaces the text
of that buffer, as one edit. `readFifo()` polls like `runPipeCommand()`:

- After `kFifoAbortPromptDelay` (2 s) without data, it asks "Still waiting for <path>. Abort? y/n". The FIFO may
  have no writer yet, or a writer that sends nothing.
- `y` aborts the open. `n` waits another 2 s.
- A SIGTERM aborts the open too, and the loop acts on the request later.
- It redraws on SIGWINCH.

#### Session state

`Screen` holds the session-wide state that does not belong to any one buffer:

- the active prompt
- the find state
- the paste ring
- the X11 `Clipboard` adapter
- the timed status message
- the `Bookmarks` store (`_bookmarks`, `src/bookmarks.hpp`)

Bookmarks are global across buffers and last only for the session. A `Buffer` is per buffer and a `BufferView` is
transient, so `Screen` is their only sensible owner. `openSetBookmarkPrompt()` and `openJumpBookmarkPrompt()`
(bound to `C-x b` and `C-x j`) ask for a single slot character. The jump calls `jumpToBookmark()`. It switches the
buffer of the focused panel if needed, and clamps the stored point to the current contents of the target buffer.
`renameBufferAndRekeyBookmarks()` wraps every `BufferManager::rename()` call site (both Save-As paths), so the
bookmarks of a renamed buffer follow it under the new name. cx refuses to set a bookmark in a buffer that the manager
cannot find by name: the singleton overlays `*help*`, `*buffers*` and `*paste ring*`. A bookmark into one of them
could never work.

The per-view display toggles (soft wrap, whitespace markers) live on `BufferView`, so that each panel can have its
own settings. After every keystroke, `loop()` calls `syncPrimarySelection()`, which copies the active mark region to
the X11 PRIMARY selection. It does nothing when there is no mark or no graphical session.

#### Read-only overlays

`Help`, `BufferList` and `PasteRing` switch the focused panel to a read-only buffer, and keep the previous buffer in
`Panel::prevBuf`. `Esc-Esc`, `q`, or the same overlay key a second time, restores the previous buffer from there.
That also restores its saved cursor, mark and viewport. `handleBareEscEsc()` does the overlay-dismissal part of
`Esc-Esc`. It clears the mark only when no overlay is up.

`updateFocusPointers()` binds `_buf` and `_view` to the focused panel again after layout changes: split, close,
focus, kill or overlay restoration. Every change of focus goes through it, so it also warns when the file of the
newly focused buffer changed on disk.

#### Shared helpers

`bufferWorkDir()` returns the parent directory of the focused buffer's file. For a buffer without a path, it returns
the working directory of the process. Every external command that `runPipeCommand()` starts runs in this directory.

#### Line endings

When cx opens a file with non-Unix line endings, `checkLineEndings()` reads the `LineEndingStyle` that
`TextBuffer::loadFromFile()` set. For CRLF or lone CR, it asks the user whether to translate. For mixed endings, it
shows a "mixed line endings" status message. Reading the flag acknowledges it, so cx flags the same buffer only once
per load.

#### Prompts

The `Prompt` struct (`src/prompt.hpp`) represents a prompt. It carries:

- a label
- callbacks for autocomplete, result, abort, Tab and special keys
- cached display state
- a UTF-8-aware edit cursor with single-line editing operations: insert, character and chunk deletion, and
  character, word and Home/End movement
- a mark. `Screen::handlePromptAction` cuts, copies and pastes its region through the shared paste ring, with the
  same shortcut-table actions as in the buffer.

Callbacks that replace `input` as a whole (Tab, Right-arrow completion, history, special keys) call
`inputReplaced()`. That leaves the cursor at the end and drops the mark.

`drawModeline()` draws the cell under the cursor in normal video against the reverse-video bar, and the region in
underlined normal video. It returns the cursor column. `draw()` parks the hidden terminal cursor there, because on
top of that cell the cursor would invert it back into the colours of the bar.

An `openXxxPrompt()` helper constructs each prompt type (e.g. `openGoToLinePrompt()`, `openSwitchBufferPrompt()`,
`openConfirmPrompt()`). `PromptHistory` (also in `prompt.hpp`) records input history per label. Up and Down
navigate it.

These flags change how a prompt behaves:

- Ctrl-q sets `Prompt::quoteNext`. The next key goes into the input as it is (Enter as a newline).
- The find, replace and regex prompts set `Prompt::multiLine`. A paste keeps all its lines, and Tab inserts a tab.
  `promptText()` shows a newline as `\n` and a tab as `\t`.
- Enter on an empty input closes a prompt without calling `onResult`, unless the prompt sets
  `Prompt::acceptEmpty`. The replacement prompts of Esc-% and Esc-Y set it, so an empty replacement deletes each
  match.

Find matches a pattern with a newline exactly across lines, the same way as Replace (`findNextMatchInBuffer` /
`findPrevMatchInBuffer`). `onFindMatch(from, to)` selects the match.

---

### `BufferSink` (`src/buffer/buffer_sink.hpp`)

This abstract interface has a single method, `setBuffer(Buffer&)`. It decouples `BufferManager` from `Screen`, so
unit tests can supply a stub sink without a real terminal.

---

### `BufferManager` (`src/buffer/buffer_manager.hpp/.cpp`)

`BufferManager` owns these buffers:

- all `TextBuffer` instances, keyed by canonical path or synthetic name
- all `DirBuffer` instances, keyed by canonical directory path
- the singleton `HelpBuffer`
- the `BufferListBuffer`, `PasteRingBuffer` and `GrepBuffer`, which it creates when they are first needed

It keeps an MRU list for buffer switching.

Key responsibilities:

- `open()` canonicalizes a path. It opens a `TextBuffer`, or returns the existing one.
- `openDir()` canonicalizes a directory path. It opens a `DirBuffer`, or returns the existing one. For an existing
  one, it reads the listing again (`DirBuffer::reload()`) and keeps the saved cursor on its entry.
- `grepBuffer()` creates the singleton `GrepBuffer`, or refreshes it, with a new result set.
- `create()` creates a new empty `TextBuffer` with a synthetic name.
- `kill()` removes a buffer. It always leaves at least one buffer alive.
- `completions()` returns the candidates for the switch-buffer prompt. They cover both open buffers and filesystem
  entries. It caches the directory listing behind them for each directory, and reads it again when the
  modification time of the directory changes.
- `displayName()` returns a short, unambiguous name for the status line and the buffer list. That is the filename
  when it is unambiguous, and the full path otherwise.
- `touch()` / `secondMRU()` manage the MRU list for the default buffer switch.
- `rename()` gives a buffer a new key in the map after Save As. It canonicalises the new path as `open()` does. It
  refuses (returns false) if another buffer already has that path.

---

### `Buffer` (`src/buffer/buffer.hpp/.cpp`)

`Buffer` is the abstract base for all buffer types. It owns *document content only*: line storage, filename, mode,
modified flag, document version, and the syntax cache for each line. All *view state* lives in `BufferView`: cursor
position, viewport, mark, selection highlight and the caches for each view. So several views can share the same
`Buffer` for split-window editing.

The text is in a `LineStore` (see below) of `LinePtr`s. `LinePtr` is `std::shared_ptr<std::string const>`, one
string per line, without the newline. The store shares its chunks and lines with the `VersionHistory` snapshots. So
to capture or restore a snapshot, cx copies one pointer per chunk (O(N/512)). It does not copy one pointer per line,
and never a string.

Changes go through `Buffer::mutLine(idx)` → `LineStore::mutableLine`. That call detaches a shared chunk and then a
shared string, and then hands out a mutable reference. New lines come from `makeLine()`. `lineContent(idx)` and
`lineCount()` keep their signatures. `lines()` returns `LineStore const&`. An empty buffer has a single empty line.
A file that ends in `\n` has an empty entry at the end. Loading collects the lines into a vector and moves it into
the store, which keeps the line strings next to each other in memory.

Public API:

- Position-explicit editing: `insertAt(cp, point)`, `deleteAt(point)`, `deleteBefore(point)`,
  `insertText(text, point)`, `deleteRegion(from, to)`, `extractRegion(from, to)`. Every editing method takes the
  cursor position as a parameter and returns the new position. `Buffer` never reads or writes view state.
- Rendering: the virtual `render(out, rows, cols, RenderParams)` produces an ANSI escape string for the visible
  part of the buffer. The string includes syntax highlighting (from the tokenizer of the mode), region highlighting
  (inverse video when a selection mark is active) and find highlighting. It returns the screen `{row, col}` of the
  cursor.
  - `RenderParams` bundles the view-state inputs (point, viewFrom, mark, highlight, extraCursors, showWhitespace,
    wrap, row and col offsets) and the `Theme`. The `Palette` of the theme (`src/theme.hpp`) supplies every colour.
  - `RenderParams::extraCursors` is a sorted span of secondary cursor positions. It is empty in single-cursor
    editing. Each one that falls in the viewport shows its character cell in reverse video (a space at the end of
    a line). The hardware terminal cursor stays on `point`.
  - Nothing is drawn outside the panel. A cluster that the left edge of a horizontally scrolled row cuts is drawn
    as blanks. A cluster wider than a whole wrapped row (an escape in a panel a few columns wide) is cut to fit.
  - In no-wrap mode, a separate loop measures the part of a line that is scrolled off to the left. A long line
    scrolled far right spends its time there.
- Screen row query: the virtual `screenRow(cols, point, viewFrom, wrap)` reports where a point is relative to the
  start of a viewport. `BufferView::ensureVisible` and similar methods use it.
- Mode: the `BufferMode` enum (`Txt`, `Cpp`, `Md`, `Jsn`, `Dif`, `Sh`, `Yml`, `Tml`, `Py`) controls syntax
  highlighting, auto-indent and formatting. `setMode()` changes it and invalidates the syntax cache.
- Line-number gutter: `RenderParams::leftGutter` (in screen columns) reserves space at the left of the panel for
  right-aligned, 1-based physical line numbers. `Buffer::render` honours it. The derived buffers Help, Dir,
  BufferList and PasteRing opt out through `supportsLineNumberGutter()`, so the per-view toggle silently does
  nothing on overlays. The state of the toggle lives in the view (`BufferView::showLineNumbers`,
  `lineNumberGutterWidth`).
- Document version: every edit increments `docVersion()`. So a `BufferView` can detect a change that another view
  of the same buffer made, and call `clampToBuffer()` to check its cached positions again.
- Saved view state: for buffer switching, a buffer can keep a `SavedViewState` ({point, viewFrom, mark}) through
  `setSavedView()` / `savedView()` / `clearSavedView()`. `BufferView::fromSavedState()` restores it later.
- Version-history hooks: `BufferView` calls `recordVersion(cursor)` after every successful edit. The replace-all
  helpers, which edit the buffer directly, call it too. The default does nothing, so read-only buffers ignore it.
  `TextBuffer` overrides it to append a snapshot of the whole buffer to its `VersionHistory`. The snapshot shares
  the chunks of the line store, so callers do not need to describe what changed. The history-browsing interface
  (`browsingHistory()`, `setBrowsingHistory()`, `versionHistoryInfo()`, `applyVersionAt()`,
  `truncateHistoryAfter()`) is virtual on the base class. So `BufferView` can drive history navigation through the
  abstract type, without a dependency on `TextBuffer`.

`readonly()` guards the editing methods. It is pure virtual, and derived classes override it. Any edit sets the
`_modified` flag, and a successful `save()` clears it. While history browsing is active, `TextBuffer` reports
`readonly() == true`. So all editing entry points do nothing, without a special case in each method.

Insert or overwrite is a flag for each buffer (`_overwriteMode`, toggled with `toggleOverwriteMode()`). It lives on
the base `Buffer`, so every view of the buffer shares it. It changes only the editing behaviour: when the flag is
set, `BufferView::insert()` deletes the character under the cursor before it inserts. It also has two visual cues:
the `I`/`O` modeline indicator and the terminal cursor shape (see `Screen`). `drawModeline` puts the live cursor
count of the view in front of that indicator, as `<count>@`. The count is blank with one cursor, and `***@` above
999. The count and the `@` take a fixed-width field, so the `I`/`O` never changes column as cursors come and go.

Caching:

- `_synCache` is the syntax cache, one entry per line. An edit invalidates it from the edited line down, until the
  tokenizer state matches again. `cachedLineTokens(line)` exposes it to other subsystems. For example, bracket
  matching reuses the tokens that the renderer already computed.
- `_bracketCache` holds the last `findMatchingBracket()` result, keyed by point and `docVersion`. Repeated render
  passes reuse the match and do not scan again.

Conflict detection:

- `scanForConflicts()` walks the lines and sets `_hasConflicts` if any line is a 7-character conflict marker
  (`<<<<<<<`, `=======`, `>>>>>>>`). `TextBuffer` runs it on load, revert and save. When the flag is set, the
  syntax-cache pass puts a `ConflictMarker` token over each conflict-marker line, whatever the mode. So even a `Txt`
  buffer shows merge regions with their own backgrounds.

### `BufferView` (`src/buffer/buffer_view.hpp/.cpp`)

`BufferView` owns all the state of one logical view of a `Buffer`: the point (`BufferPoint`), the mark, the
secondary cursors, the viewport (`ViewFrom`), the wrap mode, the desired column for vertical movement, the find
highlight, and the caches for the view (`PointColCache`, `RowBoundsCache`). Several `BufferView`s can share the
same `Buffer`. Each has its own cursor and viewport.

All movement, editing and render operations for the user are methods on `BufferView`:

- Movement: `moveLeft/Right/Up/Down`, `moveHome/End`, `moveWordLeft/Right`, `moveParagraphUp/Down`,
  `movePhysicalUp/Down`, `pageUp/Down`, `gotoLine`, `gotoMatchingBracket`, `setPoint`.
- Editing: `insert`, `deleteAtPoint`, `deleteBeforePoint`, `deleteChunkAtPoint`, `deleteChunkBeforePoint`,
  `insertText`, `deleteRegion`. These call the position-explicit methods on `Buffer` and adjust the mark locally.
  - A chunk delete first finds its span and then removes it as one edit (`deleteSpan`). So it is a single history
    entry, however long the chunk is.
  - `replaceRegion(from, to, text)` swaps a region for new text as one edit and one history entry. `Screen` uses
    it wherever it replaces text (find-and-replace, piping a region, paste cycling). So an undo never lands on a
    state where the old text is gone and the new text is not there yet.
  - `replaceLines(fromLine, toLine, lines, cursor)` does the same for whole lines, when the new lines were computed
    from all of the old ones and are mostly equal to them. Reflow and the formatter's output for the whole buffer
    use it. It leaves the equal lines at either end alone, so they stay shared with earlier history versions and
    keep their syntax cache. It rewrites only the span between them.
- Case conversion: `convertCase(CaseOp)` uppercases, lowercases or capitalizes the active region. With no mark set,
  it changes the word at or after the cursor. The transform is byte-wise ASCII, which is UTF-8-safe and keeps the
  length. It applies as a single `deleteRegion`+`insertText` edit.
- Reflow: `reflowParagraph(width, ReflowTables)` refills the paragraph at the cursor, or every paragraph in the
  region. It keeps list bullets, common indentation and blank lines.
  - With `ReflowTables::Markdown`, which `Screen` passes for `md` buffers, it formats GFM tables through
    `modes/md/table.hpp` instead. It leaves fenced code blocks (`modes/md/fence.hpp`) untouched.
  - It aligns the cursor's table and wraps the cursor's column.
  - Paragraphs stop at adjacent tables.
  - A region widens to cover whole tables, which it only aligns.
  - The result is one `deleteRegion`+`insertText` edit (one undo step) over only the span of lines that differ. So
    unchanged lines keep their storage, which they share with earlier history snapshots. When the text does not
    change, there is no edit.
- Mark and region: `toggleMark`, `setMark`, `clearMark`, `extractRegion`.
- Viewport management: `ensureVisible`, `scrollUp/Down`, `scrollViewNearEnd`, `currentLineWraps`. `ViewFrom`
  describes the viewport. It holds a `RowIdx line` and a sub-position that depends on the mode: `ByteOffset` in
  wrap mode (scrolling within a line), or `VisualCol` in no-wrap mode (horizontal scrolling).
- Rendering: `render()` builds a `RenderParams` from the state of the view and the `BufferRenderArgs` for the frame
  (focus, panel offsets, theme) that `Screen::draw()` passes. It then calls `Buffer::render()`.
- Highlight: `setHighlight/clearHighlight` for find highlighting.
- Cursor helpers: when the cursor is in the leading whitespace, `adjustCursorToIndent()` moves it to the first byte
  that is not a space.
- History mode: `enterHistoryMode()`, `stepHistory(delta)`, `exitHistoryModeCommit()` and
  `exitHistoryModeCancel()` give read-only browsing of the version history of the buffer.
  - Entering sets the buffer to read-only through `setBrowsingHistory(true)`.
  - Stepping calls `applyVersionAt()` and updates the cursor.
  - Committing (Ctrl-u, Enter) truncates any history after the selected version and allows editing again.
  - Cancelling (Esc-Esc) shows the newest version again, puts the cursor back where it was on entry, and drops
    nothing.
  - The read-only flag lives on the buffer, so a view must leave history mode before it is dropped.
    `Screen::setBuffer()` cancels history mode, so opening an overlay never discards versions.
  - Only one view of a buffer can browse at a time, because the shown snapshot belongs to the buffer.
    `enterHistoryMode()` returns false while the buffer is already browsing.
  - `inHistoryMode()` / `historyIndex()` expose the current state to `Screen` for the status line.
- Multiple cursors: `_extraCursors` holds the secondary cursors. They are kept sorted and free of duplicates, and
  are never equal to `_point`. `multiCursor()` *is* "that vector is not empty", so there is no separate mode flag
  that could drift. `selectAllOccurrences()` (`Esc-*`) and `addCursorDown/Up()` (`Ctrl-Shift-Down/Up`) create them,
  and `collapseCursors()` drops them.
  - Editing: `insert`, the deletes, the chunk deletes and `insertText` go to `multi*` drivers. Each driver
    snapshots every cursor into `_batch` (sorted) and visits the cursors in ascending order. It makes primitive
    edits through the position-explicit API of `Buffer`. Every edit happens at or after the visited cursor, so the
    unvisited tail shifts lazily. `BatchShift` is an anchor pair: a position from before the batch, and where it is
    now. `batchVisit()` uses it to map each cursor once, and also detects cursors that merged onto the cursor before
    them. Only a backward deletion can reach visited cursors, and it collapses them onto its start. A keystroke is
    O(N) in the cursor count.
  - History: the batch ends with exactly one `recordVersion(point)`. So one keystroke is one undo step, whatever the
    cursor count.
  - Movement: the parallel commands are thin wrappers. Their `*Impl` body runs once for the primary cursor, and
    then once for each extra cursor (`moveExtras()`), with that extra cursor installed as `_point` for the moment.
    Commands that jump somewhere unrelated collapse to the primary cursor first: `setPoint` (and so find,
    bookmarks and mouse clicks), `gotoLine`, paging, paragraph moves and bracket match. Setting the mark, region
    edits and entering history mode collapse too. The other way round, adding a cursor clears the mark, because
    selections do not belong to single cursors. Every batch edit clears it too, because batch edits never shift it.
  - Extra cursors are deliberately **not** kept in `Buffer::SavedViewState`. A buffer switch or a panel close
    collapses to the primary cursor.
- Consistency: `clampToBuffer()` clamps the point, mark, extra cursors and viewport to valid positions again after
  the buffer changed (e.g. through another view or `revert()`). The view calls it automatically when it notices a
  `docVersion` mismatch. `snapToCodepoint()` (`utf8.hpp`) snaps every clamped byte offset (and `setPoint()`) back to
  a code-point boundary. So a stale position can never split a multi-byte character. The view's own edits adopt
  the new version through `syncAfterEdit()`, which also keeps the viewport in range.

---

### `TextBuffer` (`src/buffer/text_buffer.hpp/.cpp`)

`TextBuffer` is the concrete editable buffer, backed by a file on disk. It reads the file on construction.

- `save()` writes the lines back with a truncating binary write. It returns `std::expected<void, std::string>`, with
  an error message on failure.
- `writeCopy(path)` writes the same bytes to a new owner-only file instead. It uses `umask` and
  `std::ios::noreplace`, so the file is private from creation, and an existing file or symlink of that name stays
  untouched. It does not change the buffer's path, modified flag or disk stamp. The emergency save uses it.
- `revert()` reloads the buffer contents from the file and bumps `docVersion`. It does *not* touch view state.
  Callers that need to move the cursor after a revert reset it themselves. For example, `Screen` calls
  `_view->setPoint(0,0)` and clears the mark for the user's `Revert` action.
- `setPath()` supports the Save As rename.
- `detectMode()` sets the `BufferMode` from the file extension:
  - `.c`, `.h`, `.cpp`, `.hpp` → `Cpp`
  - `.md` → `Md`
  - `.json` → `Jsn`
  - `.diff`, `.patch` → `Dif`
  - `.sh`, `.bash`, `.zsh`, `.ksh` → `Sh`
  - `.yml`, `.yaml` → `Yml`
  - `.toml` → `Tml`
  - `.py`, `.pyi`, `.pyw` → `Py`
  - otherwise `Txt`

  When the extension is unknown, it looks at a leading `#!` line for an interpreter as a fallback
  (`bash`/`zsh`/`ksh`/`dash`/`sh` → `Sh`, `python` → `Py`). It reads only line 0, so `detectMode()` now runs after
  `loadFromFile()` in the file constructor.

`TextBuffer` also keeps the mtime and size of the file on disk. It captures them on load and after a successful
`save()`. So `changedOnDisk()` can warn when another process changed the file. `Screen::Save` checks that flag
before it overwrites the file, and asks the user when it is set. `noticeDiskChange()` does the same check, but
reports each state of the file only once (until the next load or save). `Screen::updateFocusPointers()` uses it to
warn in the status line when a buffer gets focus and its file changed.

`loadFromFile()` captures the line-ending style in a `LineEndingStyle` enum (`Unix`, `Crlf`, `Cr`, `Mixed`). The
`\r` bytes stay in the line strings, so the user can still edit them. `Screen` reads the flag through
`lineEndingStyle()`. For a non-Unix file, it asks the user whether to normalize, and calls `normalizeLineEndings()`
on consent. `acknowledgeLineEndingStyle()` clears the flag, so cx does not flag the same load twice. For `Mixed`
files, cx shows a status warning and does not ask, because they cannot be translated unambiguously.

`TextBuffer` owns a `VersionHistory _history` and a `bool _browsingHistory` flag. It captures the history:

- on construction
- on every successful edit, through the inherited `recordVersion()` hook
- on `save()` and `revert()`, so that the state on disk is a snapshot that the user can restore

`readonly()` returns `_browsingHistory`, so the buffer cannot change while a view steps through history. When the
browsing view shows the newest version again, the buffer gets back the `modified()` flag that it had when browsing
began. So stepping back and returning (or cancelling) leaves a saved buffer unmodified.

---

### `HelpBuffer` (`src/buffer/help_buffer.hpp/.cpp`)

This read-only buffer renders the key-binding reference. It overrides `render()` and `screenRow()`. When the
terminal is wide enough, it lays out the command and movement sections side by side. Otherwise it stacks them. It
generates the command section from the shortcut binding table, through `formatShortcut()` and
`descriptionForAction()`. So the help display always matches the actual keybindings.

---

### `BufferListBuffer` (`src/buffer/buffer_list_buffer.hpp/.cpp`)

This read-only buffer shows the open buffers as a directory tree, built from a trie. Nodes with a single child are
collapsed. It overrides `render()` to draw the selected row in reverse video, padded with spaces up to the gutter,
so the whole row is highlighted. `selectsRows()` returns true, so `Screen::draw()` hides the terminal cursor while
the buffer has focus. The directory and grep buffers do both the same way. Rows are clipped to the panel width
(`clipToVisualWidth()`), as in the directory and grep buffers. When the user presses Enter, `Screen` calls
`selectedName()` to get the buffer name under the cursor.

---

### `DirBuffer` (`src/buffer/dir_buffer.hpp/.cpp`)

This read-only buffer shows a directory listing with permissions, owner, group, size and modification time. It
supports:

- navigation into subdirectories and to the parent (the `.` and `..` entries)
- toggling the visibility of hidden files (dotfiles)
- marking entries for deletion and deleting them all at once
- renaming the selected entry (`r`) and creating subdirectories (`m`)

Each entry is a `DirBuffer::Entry` struct with metadata from the filesystem. `refresh()` rebuilds the listing from
disk and bumps `docVersion()`, so views of it clamp their positions again. `reload()` reads a listing that may be
stale again. cx calls it whenever the directory is opened again. The cursor and the deletion marks follow their
entries by name, because positions shift when files come and go.

---

### `GrepBuffer` (`src/buffer/grep_buffer.hpp/.cpp`)

This read-only buffer lists find-in-files results. It has a one-line header that describes the search, and then one
`path:line: text` row per hit. `refresh()` rebuilds the listing from a new result set. `selectedHit()` maps a cursor
position below the header back to a `GrepHit`, so `Screen` can open the file when the user presses Enter.

The free function `parseGrepOutput()` turns the raw stdout of either backend into `GrepHit` values:

- cx runs both backends with a NUL after the filename (`git grep -z`, `grep -Z`). This keeps paths that contain `:`
  or a newline unambiguous, because the parser finds the path by its NUL before it looks for the end of the record.
- The backends differ in whether a NUL or a `:` ends the line number. The parser accepts either.
- The parser skips malformed records and does not abort.
- Consecutive hits in one file share a `GrepFile` (absolute and display path). A `std::filesystem::path` per hit
  cost about 800 bytes, which was 3.4 GB for 4M hits.
- It builds the display rows once, directly into the `LineStore` of the base class.
- When the output limit of `runGrep()` cuts a result set short, the header says so ("search stopped at the limit").

Backend selection, argument construction and opening a hit live in `Screen` (`runGrep()`, `searchStartDir()`,
`openGrepHit()`), not here. So this class only displays a result set that is already computed.

- Inside a git work tree, `git grep` runs from the repository root. So it honours `.gitignore`, and it searches
  only tracked files.
- Otherwise, `grep -r` runs from `searchStartDir()`. That is the browsed directory for a `DirBuffer`, or else the
  parent of the current file, or else the working directory of the process.
- `openGrepHit()` shows the target in the other panel. If only one panel is open, it first splits it vertically.
  So the listing stays visible while the user walks through several matches.

---

### `PasteRing` (`src/paste_ring.hpp`)

This is a fixed-capacity ring buffer for cut or copied text. It stores entries as `std::string`, with embedded
`'\n'` for line breaks. Index 0 is the most recent entry. `Screen` uses it for cut, copy, paste and paste cycling.

---

### `Clipboard` (`src/clipboard.hpp/.cpp`)

This class gives best-effort X11 clipboard integration. On first use, it checks for `DISPLAY` and for `xclip`
(preferred) or `xsel` on `$PATH`. If neither tool is available, all methods silently do nothing, so the editor
still works on a tty without a graphical session. The project rules forbid linking `libX11`, so all interaction
goes through these external tools.

`Screen` uses it in these places:

- `setPrimary()` after every keystroke (in `loop()`, through `syncPrimarySelection()`). So the active mark region
  follows the X11 PRIMARY selection, which mouse-drag selections fill and middle clicks paste. The class compares
  each text with the last text it published, so cursor moves that do not change the selected text cost no forks.
- `setClipboard()` on cut and copy, so other applications can get the text through the standard Ctrl-C/Ctrl-V
  clipboard
- `getClipboard()` on paste, so copies from other applications come into the editor (and onto the paste ring)
- `getPrimary()` on a middle click, so the PRIMARY selection pastes in at the cursor

`getClipboard()` and `getPrimary()` share a private `getSelection()` helper.

Writes use a double fork to detach. The parent forks an intermediate child. That child forks again and exits, and
init reaps the grandchild xclip/xsel. This keeps the input loop responsive (about 1 ms per write), however slowly
xclip starts. Reads are synchronous, because the caller (Paste) needs the result at once.

Errors go to `~/.cx-errors` through `error_log`. The user never sees them.

---

### `PasteRingBuffer` (`src/buffer/paste_ring_buffer.hpp/.cpp`)

This read-only buffer shows the contents of the paste ring. It works like `BufferListBuffer`: `refresh()` rebuilds
the lines from the current state of the ring.

---

### `LineStore` (`src/buffer/line_store.hpp/.cpp`)

`LineStore` is an indexed sequence of `LinePtr`s, kept in shared copy-on-write chunks of up to 2×T pointers.
T is 512 by default. It is a constructor parameter, so tests can use tiny chunks. Every chunk but the last holds at
least T/4.

- To copy a store, cx copies one pointer per chunk, and the copies share every chunk. The first edit of a shared
  chunk clones it, and `mutableLine(i)` then also detaches a shared line string.
- A binary search over the first line numbers of the chunks finds line `i`. A cached last chunk makes sequential
  access O(1).
- Edits cost O(T + N/T). Large inserts build whole chunks, and large erases drop whole chunks.
- The free `makeLine()` helpers create line strings that are not const behind the `string const` pointer. This
  makes the in-place edit of an unshared line in `mutableLine()` well defined.

The header states these assumptions:

- The store is single-threaded, because of the lookup cache and the `use_count()` sharing test.
- References and iterators stay valid only until the next change.

Bulk loads move a `std::vector<LinePtr>` in, so cx allocates the chunk arrays after the lines. (Measurements showed
that 8 KB arrays between the line strings halved the speed of later scans.) `operator[]` caches the range and array
of the last chunk, so a nearby read costs one comparison and one load. The iterator holds the element range of the
current chunk. `Buffer` uses the store for its lines, and `VersionHistory` uses it for full snapshots.

---

### `VersionHistory` (`src/buffer/version_history.hpp/.cpp`)

This is an append-only history of buffer snapshots. `TextBuffer` uses it for undo and version browsing. Each
`VersionSnapshot` records:

- the lines of the buffer at the time of capture, as a `LineStore` copy that shares the buffer's chunks
- a timestamp
- the cursor position

Every version is a complete snapshot. A capture copies one pointer per chunk (O(N/512)), whatever the edit was.
Versions keep only the chunks and lines that differ between them. So memory grows with what was edited, not with
file size × versions. `linesAt(idx)` returns a copy of the store of a version (at the same cost) without changing
the history. `timestampAt(idx)` and `cursorAt(idx)` are O(1).

Pruning runs whenever `_versions.size()` exceeds `kMaxVersions`. It drops every other entry among the oldest
`kThinCount`. Snapshots are independent, so dropping one needs no fix-up of its neighbours. It costs one
reference-count decrement per chunk, plus freeing what only that version held.

Public surface: `capture(lines, cursor)`, `linesAt(idx)` / `timestampAt(idx)` / `cursorAt(idx)`,
`truncateAfter(idx)` (to commit a rewind in history mode), and `size()` / `empty()` / `clear()`.

---

### `Panel` (`src/terminal/panel.hpp/.cpp`)

A panel is one pane of the terminal. It owns:

- a `BufferView`
- an integer id
- a `PanelRect` with its absolute position and size
- a `Buffer* prevBuf` slot, which the read-only overlay flow uses (see `Screen` above)

`setBuffer()` switches the panel to a different buffer. It can restore saved view state through
`BufferView::fromSavedState()`. `saveViewState()` keeps the cursor, mark, viewport and wrap state before the view is
destroyed. cx calls `saveViewState()` whenever a panel is about to drop its `BufferView` (a buffer switch or a panel
close), not only when the last view is destroyed.

---

### `PanelLayout` (`src/terminal/panel_layout.hpp/.cpp`)

`PanelLayout` manages a set of panels in a binary split tree. Each leaf owns a `Panel`. Each internal node records a
split direction (`Vertical` / `Horizontal`). `resize()`, `splitFocused()` and `closeFocused()` compute the tree
again and give each panel an absolute `PanelRect`.

Key operations:

- `splitFocused(dir, buf)` replaces the focused leaf with a split node. The original panel stays left or top, and a
  new panel goes right or bottom. It returns `nullptr` if the split would make panels smaller than the minimum size
  (10 cols, 3 rows). Panels are numbered from 1 upward. `FocusPanel1`..`FocusPanel6` go to the first six panels in
  tree order, which is the practical maximum.
- `closeFocused()` removes the focused leaf and promotes its sibling. It gives focus back to the most recently used
  panel, from an MRU history. If there is none, it focuses the first panel in tree order.
- `focusPanel(id)` switches focus by panel id and updates the MRU history.
- `forEachPanel(fn)` visits every panel in tree order.

---

### `BufferSink` / `BufferManager` interaction

`BufferManager` holds a `BufferSink&`. It calls `setBuffer()` on it whenever it switches to a new buffer: from
`open`, `openDir` and `create`, for the initial buffer of the constructor, and for the replacement buffer of `kill`.
`Screen` implements `BufferSink::setBuffer()`: it updates its `_buf` pointer and calls `draw()`. The dependency goes
one way only, so `BufferManager` never knows the concrete `Screen`.

---

## Syntax highlighting

Syntax highlighting depends on the buffer mode. The code lives in `src/buffer/modes/`.

### C/C++ mode (`modes/c/syntax.hpp/.cpp`)

The tokenizer recognises:

- keywords
- comments, both line and block
- strings: regular strings, character literals, and raw strings with `R"delim(...)delim"`
- value literals (`Number`):
  - numeric literals: integer and float, `0x`/`0b` prefixes, `'` digit separators, hex floats, and
    `u`/`l`/`f`/`z`/user-defined suffixes
  - the boolean and null literals `true`/`false`/`nullptr`. They are values and not keywords, so they share the
    numeric colour.
- operators
- brackets: parens, brackets, braces and angles

It keeps multi-line state (`CSyntaxState`) for block comments and raw string literals. It also provides
`findMatchingBracket()` for jump-to-matching-bracket.

### Markdown mode (`modes/md/syntax.hpp/.cpp`)

The tokenizer recognises headings (`# ...`), bold (`**...**`, on one line) and links (`[text](url)`).

Links can span lines. `MdSyntaxState::mdLink` (`MdLink::Label`/`Url`) carries an open `[...` label or `](...`
destination across the line boundary. The link has the Link colour until it closes. If it never closes, the colour
stops where its block ends, so an unmatched `[` in prose does not colour the rest of the file. A block ends where
Alt-q starts a new one:

- at a blank line
- at a fenced code block
- at a list item (`listMarkerLength()`)
- at a table row (`MdSyntaxState::table`, an `MdTableScan`)

A table row is a block of its own, so a `[` that does not close on the row is plain text there. Only the header row
is not recognised, because the delimiter row below it is what makes it a header.

More rules:

- Headings are suppressed while a link continuation is open.
- Bold works on one line only.
- The tokenizer skips inline code spans (`` `...` ``), so it does not tokenise markup inside them. Most
  importantly, a stray `[` there cannot open a link. So an unbalanced bracket in code does not carry link
  highlighting onto the next lines.
- Fenced code blocks get no markup tokens at all. For example, a `# comment` in a shell snippet is not a heading.
  `MdSyntaxState::fence` (an `MdFence`) tracks the open fence from line to line.

### Markdown tables (`modes/md/table.hpp/.cpp`)

These are pure functions, with no dependency on a buffer or a view.

- `findPipeRun()` returns the run of lines that contain a `|` around a line, and the table header inside that run.
  Callers reuse the result for every line of the run, which keeps detection linear.
- `findMarkdownTable()` narrows the run to the line range of the GFM pipe table around a line: a header, then a
  delimiter row with the same cell count, then body rows. Every line holds an unescaped `|`.
- `formatMarkdownTable()` pads every cell to the `visualWidth` of its column and resizes delimiter rows. It keeps
  their `:` alignment markers. Given a cursor, it also wraps the cursor's column into the width that the other
  columns leave free in the target width:
  - That width is at least the longest word, the header, or min(20, the widest cell).
  - Code spans and links are never split.
  - A GFM row is one line, so a wrapped cell continues in body rows whose first cell is empty.
  - Column 1 and the header are never wrapped.
  - A body row that is completely empty is a spacer, and it stays.
  - Rows can have indentation and block-quote markers (`> `), and every output row gets them. The header can have
    a byte-order mark.
  - The returned cursor keeps its logical cell, and its offset in non-blank bytes within the text of that cell.
- `MdTableScan` is the same table test, for a caller that sees one line at a time (the tokenizer). It remembers the
  cell count of the line above, and does not look at the line below. So it reports the delimiter row and the rows
  under it, but not the header.

### Markdown list markers (`modes/md/list.hpp`)

`listMarkerLength()` recognises `- `, `* `, `+ `, `1. ` and `1) `. Reflow uses it in every mode, to reflow each list
item on its own. The Markdown tokenizer uses it to end an open link at the next item.

### Markdown fences (`modes/md/fence.hpp/.cpp`)

`MdFence` is the line-by-line state of "which fenced code block am I in" (``` or ~~~, with a closing run at least
as long). It is the only place that knows the rules. `markdownFencedLines()` runs it over a whole buffer for reflow,
which leaves fenced lines untouched. The Markdown tokenizer carries one in its state for each line.

### Diff mode (`modes/dif/syntax.hpp/.cpp`)

The tokenizer classifies whole lines by their prefix: `+` (add), `-` (remove), `+++`/`---` (file headers) and `@@`
(hunk headers). It needs no multi-line state, so it takes none.

### JSON mode (`modes/jsn/syntax.hpp/.cpp`)

The tokenizer recognises strings, numbers, the value literals `true`/`false`/`null`, and the brackets `[]` and
`{}`. The value literals have the `Number` colour, like numeric literals, and JSON emits no `Keyword`. It needs no
multi-line state, so it takes none. It provides `findMatchingBracketJson()` for jump-to-matching-bracket. That
function matches only `[]` and `{}`, and skips brackets inside strings.

### Shell mode (`modes/sh/syntax.hpp/.cpp`)

The tokenizer recognises:

- `#` comments
- single-quoted, double-quoted and backtick strings
- the reserved words (`if`/`then`/`fi`/`for`/`while`/`do`/`done`/…)
- the parameter expansions `$VAR`/`${...}`/`$?` (the `Variable` token)
- brackets
- the operators `|` `&` `;` `<` `>` `=`

It keeps multi-line state (`ShSyntaxState`) for heredocs. `state.heredocTag` makes every following line one
`String` span, until a line equal to the tag. For the `<<-` form, which `state.heredocStripTabs` tracks, leading
tabs are stripped. `<<<` here-strings are not heredocs.

### YAML mode (`modes/yml/syntax.hpp/.cpp`)

The tokenizer recognises:

- `#` comments
- mapping keys (`Keyword`): the text before a colon that is followed by a space or the end of the line
- `'...'` and `"..."` strings
- value literals, with the `Number` colour: the plain constants `true`/`false`/`yes`/`no`/`on`/`off`/`null`/`~`/`nan`
  (case-insensitive) and numeric scalars. Mapping keys stay `Keyword`.
- `: , -` and flow brackets
- the node properties `&anchor`/`*alias`/`!tag` (`Preprocessor`)
- directives (`%YAML`) and document markers (`---`/`...`), also `Preprocessor`

It keeps multi-line state (`YmlSyntaxState`) for `|`/`>` block scalars. `state.yamlBlockCol` holds the indent of
the line that starts the block. Lines with a deeper indent, and blank lines, are one `String` span, until a
non-blank line goes back to that indent.

### TOML mode (`modes/tml/syntax.hpp/.cpp`)

The tokenizer recognises:

- `#` comments
- table headers `[a.b]`/`[[a]]`: names are `Preprocessor`, brackets are `Bracket`
- bare and dotted keys before `=` (`Keyword`). Quoted key parts are `String`.
- basic and literal strings, and their `"""`/`'''` multi-line forms
- the value literals, as `Number`: booleans, integers (`0x`/`0o`/`0b`, `_` groups), floats (`inf`/`nan`) and
  dates and times

Inside inline tables `{...}`, it recognises keys after `{` and `,`. It keeps multi-line state (`TmlSyntaxState`).
`state.tomlTriple` holds the open triple-quote character. `state.tomlDepth` counts the `[`/`{` that earlier lines
left open. So the tokenizer parses lines inside a multi-line array as values, without header or key detection.

### Python mode (`modes/py/syntax.hpp/.cpp`)

The tokenizer recognises:

- `#` comments
- strings with optional `r`/`b`/`f`/`u` prefixes, single-quoted, double-quoted and triple-quoted
- the keywords (`def`/`class`/`for`/`lambda`/…)
- numbers: `0x`/`0o`/`0b`, `_` groups, fraction and exponent, and imaginary `j`
- the value literals `None`/`True`/`False`, with the `Number` colour and not `Keyword`
- `@decorator` lines (`Preprocessor`)
- brackets, and `:` (`Operator`)

It keeps multi-line state (`PySyntaxState`) for triple-quoted strings. `state.pyTriple` holds the open quote
character, and `state.pyTripleRaw` turns off escape handling for `r'''...'''`. The `{...}` parts of f-strings stay
inside the `String` span, without recursion. `Buffer::insertAt` also keeps the leading indent in Python, as in Cpp
and Jsn.

### Bracket matching and conflict markers

`BufferView::gotoMatchingBracket()` uses the JSON matcher in JSON mode, and the C/C++ matcher in every other mode.
The C/C++ matcher treats `<>` as brackets only when they are tagged `Angle`. In plain text, Markdown, shell, YAML,
TOML and Python buffers, `<>` are `Operator` or `Normal` and never `Angle`. So those buffers get a clean fallback
that matches only `()[]{}`, for free.

In every mode, lines that match `<<<<<<<`, `=======` or `>>>>>>>` (7 identical characters) get `ConflictMarker`
tokens when `_hasConflicts` is set. `scanForConflicts()` computes the flag on load, revert and save.

### Tokens and the syntax cache

All modes share the `SyntaxToken` enum (`Normal`, `Keyword`, `Comment`, `String`, `Operator`, `Paren`, `Bracket`,
`Brace`, `Angle`, `Heading`, `Bold`, `Link`, `Number`, `Preprocessor`, `DiffAdd`, `DiffRemove`, `DiffMeta`,
`DiffHunk`, `ConflictMarker`, `Variable`). `Buffer::render()` maps tokens to ANSI colours, which it takes from the
`Palette` for `RenderParams::theme`.

`Buffer::_synCache` caches the tokenisation result of each line. An edit makes the entries from its first line down
stale, but keeps the entries below it as the *tail* (`_synCacheTailFrom`/`_synCacheTailUpto`). The edit primitives
report the lines that they replace (`noteLinesEdited()`).

- The entries below the edit stay where they are. The tail records how far their lines moved
  (`_synCacheTailShift`). Before it uses them, `fillSynCache()` moves them to the places of their lines, once,
  however many edits came in between. (Moving them on every edit made a replace-all that inserts thousands of lines
  move the whole cache once per line.)
- `fillSynCache()` tokenizes again from the top of the stale part downward. When a line ends in the same state as
  its entry already recorded, every line below it would get the same tokens again. So the rest of the tail counts as
  valid.
- So typing at the top of a file, while another panel shows its end, costs a line or two of tokenizing, not the
  whole file. (It was 98 ms per key on 600K lines before.)
- The price is one move of the kept entries per redraw after edits that changed the line count. How far the cache
  was filled limits that cost.

Each entry is a `LineSyntax<S>`. It holds the tokens of the line and the tokenizer state after the line. `S` is the
state struct of the buffer mode (above), or `std::monostate` for modes without state (txt, dif, jsn), which takes no
space.

The cache is a `SyntaxCache` (`buffer/syntax_cache.hpp`): a `std::variant` of one entry vector per state type. The
variant belongs to the buffer, so entries carry no tag, and each entry is only as large as its own mode needs: 48
bytes for C and shell, 32 for Markdown/Python/YAML/TOML, and 24 for the modes without state. The two string-valued
states are kept small:

- A C++ raw-string delimiter is at most 16 characters, so `RawDelimiter` holds it inline (17 bytes).
- A shell heredoc tag has no length limit. It is a `shared_ptr<string const>` that every line of the heredoc shares.

`ensureSynCache()` picks the state type and tokenizer of the mode and hands them to `fillSynCache<S>()`. When the
state type of the mode changed, `fillSynCache<S>()` switches the variant and drops the old entries.
`Buffer::render()` extends the cache to just past the last visible line, through `ensureSynCache()`.
`Buffer::cachedLineTokens()` exposes this cache, which grows step by step, to `findMatchingBracket()`. So
jump-to-matching-bracket looks up computed tokens for any line in the visible window. It tokenises on demand only
when a forward scan runs past the bottom.

The cheaper `advanceBlockCommentState()` is for callers that only need to keep the multi-line block-comment and
raw-string-literal state, without tokens. Unlike `tokenizeLine()`, it skips the identifier and keyword
classification, the bracket tagging and the allocation of the token vector for each byte.

---

### Supporting types and utilities

| File                                   | Contents                                                                                                                     |
|----------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `src/types.hpp`                        | `RowIdx`, `ColIdx`, `contentWidth()`, `BufferPoint`, `ByteOffset`, `VisualCol`, `ViewFrom`                                   |
| `src/byte_constants.hpp`               | Named constants for terminal input bytes (`key::` namespace) and ANSI output sequences (`ansi::` namespace, with `cursorTo()`) |
| `src/utf8.hpp/.cpp`                    | UTF-8 decoding and encoding (`decodeUtf8`, `encodeUtf8`). Character width (`charWidth`, `codepointWidth`). Clusters: `charInfoAt` measures a character and its combining marks as one unit. `clusterStart`, `prevClusterStart` and `nextClusterStart` serve all cursor movement and character deletion in `BufferView`, `Buffer` and `Prompt`, so the cursor never rests between a character and its marks. `codepointClass` (the block summary `kBlockClass` in front of one binary search). Escaped characters (invisible format characters, line and paragraph separators, C1 controls) show as `codepointEscape` escapes, and orphaned marks are drawn on `kOrphanMarkBase` ◌. Also `visualWidth`, `clipToVisualWidth`, `escapeNonPrintable`, `isWhitespace`, `readUtf8Tail`, `readUtf8Codepoint` |
| `src/unicode_tables.hpp`               | Generated by `tools/gen_unicode_tables.py` (Unicode 15.1, as in xfce4-terminal): `CodepointClass` (`Normal`, `Wide`, `Combining`, `Escaped`) and the sorted range table `kCodepointClasses`. Lookups go through `codepointClass()` in `utf8.hpp` |
| `src/prompt.hpp`                       | `Prompt` struct (label, callbacks, display cache, edit cursor, mark), `PromptDisplay`, `PromptHistory` (input history per label) |
| `src/paste_ring.hpp`                   | `PasteRing`: fixed-capacity ring buffer for clipboard history                                                                |
| `src/bookmarks.hpp`                    | `Bookmarks`: session-wide named positions (`Bookmark{bufferName, point}`), keyed by a single char in `[0-9a-z]`. `Screen` owns it |
| `src/clipboard.hpp/.cpp`               | X11 PRIMARY/CLIPBOARD integration through `xclip`/`xsel` subprocesses: lazy detection, async writes, sync reads               |
| `src/error_log.hpp/.cpp`               | Appends diagnostic messages to `~/.cx-errors` for post-mortem inspection                                                     |
| `src/search.hpp`                       | Smart-find helpers (`matchAt`, `FindFlags`, `findFirstMatch`, `findLastMatch`) for `Screen::findNext/findPrev`               |
| `src/replace.hpp`                      | Exact find-and-replace helpers (`findNextMatchInBuffer`, `findPrevMatchInBuffer`, `replaceAllForward`) for `Screen::openReplacePrompt`, and for Find with a pattern that has a newline. A pattern with newlines (`patternLines()`) matches across lines: the first piece ends a line, middle pieces are whole lines, and the last piece starts the next line (`exactMatchOnLine()`). `applyRewrites()` applies sorted matches with their new text as one delete and one insert per group of touching lines. It leaves the lines between groups (and their history sharing) alone. The multi-line replace-alls of both modes use it |
| `src/regex.hpp/.cpp`                   | `Regex`, cx's own regex engine (no `std::regex`). It has a recursive-descent parser for the dialect in `docs/regex-dialect.md` (an ECMAScript subset without back-references or lookaround), and a compiler to Pike VM instructions (`{n,m}` copies its operand, counts ≤ 1000, programs ≤ 20,000 instructions). The VM matches code points (an invalid byte is a unit of its own) in O(n·m) time, with an explicit stack instead of recursion. It runs over one line, or over a `LineSource` of lines as one text for a pattern that can match a line break (`spansLines()`: `\n`, `\s`, a class that lists one, never `.` or a negated class). The `LineSource` path is `searchLines`, `matchAtLines` and `LinesMatch` with points, and `ScanCancel` can cancel it. Semantics are leftmost-first, as in ECMAScript. It reports groups 0..9. `expand()` / `expandLines()` build replacements (`$& $0-$9 $$ $\` $'` and the escapes `\n \t \r \\`). A literal prefix lets the search skip ahead with `find()`. It keeps its scratch memory between calls (one thread only) |
| `src/regex_search.hpp`                 | Regex search and replace helpers on top of `Regex` (`compileRegex`, `bufferLines`, `findFirstRegex`, `findNextRegexInBuffer`, `stepPastEmptyMatch`, `substituteCapturesInBuffer`, `substituteCaptures`, `replaceAllRegexForward`) for Esc-X / Esc-Y. A line-spanning pattern is matched over the buffer's lines, and its replace-all collects the matches and applies them with `applyRewrites()`. Mid-line searches keep the text before them as context, and scans step past empty matches. Both replace-all helpers match each line once against its original text, rewrite it in one edit, and record one history version themselves (they bypass BufferView). Whole-buffer scans take a `ScanCancel`, which they ask once per megabyte (`ScanBudget`). `Screen::searchCancel()` stops them on Esc, Ctrl-g or a request to terminate. It keeps any other key waiting as the next input (`_typedAhead`) |
| `src/key_sequence.hpp`                 | `Key`, `KeyPress`, `decodeCsiKey`, `decodeSs3Key`: decode the CSI / SS3 sequences of cursor and editing keys (xterm, VT220/tmux and rxvt forms, with modifiers). Anything else decodes to nothing. Also the bracketed-paste markers (`isPasteStart`, `kPasteEnd`) and `normalizePastedText` |
| `src/mouse.hpp`                        | SGR mouse parser (`parseSgrMouse`, `MouseEvent`): turns `ESC[<…M/m` payloads from the input loop into structured events     |
| `src/cli.hpp`                          | `cli::parse` turns argv into a `cli::Command` (`ShowHelp` / `ShowVersion` / `OpenFiles`, which carries the `--theme`). `cli::usage` is the help text |
| `src/theme.hpp`                        | `Theme` (`Dark` / `Light`, chosen by `--theme`) and `Palette`: the truecolor SGR sequences for syntax, whitespace markers, diff and conflict lines, the find match and the Esc-_ current-line tint. `palette(Theme)` picks `kDarkPalette` or `kLightPalette` |
| `src/version.hpp/.cpp`                 | `versionText()` for `--version`. It is compiled into `cx` only, from the generated `build_info.hpp` (`cmake/build_info.cmake` runs on every build and rewrites the file only when it changes) |
| `src/file_arg.hpp`                     | `parseFileArg` splits a `path:LINE` command-line argument into a path and an optional line number                           |
| `src/fifo.hpp`                         | `isFifo` and `FifoReader`: open a FIFO without waiting for a writer (`O_NONBLOCK`) and read what is there after `poll()` reports it. `main` and `Screen::readFifo()` use it. It relies on Linux `poll()` behaviour for FIFOs: nothing until a writer connects, then POLLHUP when no writer is left |
| `src/fs_path.hpp`                      | Non-throwing path helpers for paths that the user supplies: `canonicalPath` (buffer keys in `BufferManager`/`DirBuffer`), `currentDir`, `holdsNul`. The open and Save As prompts and the rename and mkdir of the directory browser refuse a typed name with a NUL byte, because the system calls would cut it short |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup                                                                                                 |

---

### Performance characteristics

The text storage model is a chunked `LineStore` of `LinePtr`s, with one `shared_ptr<string const>` per line. It has
these characteristics:

- Overhead per line: each line is a separate `std::string` plus a `shared_ptr` control block (about 32 bytes of
  overhead, plus allocator metadata). A file with millions of short lines wastes a lot of memory and fragments the
  heap. Snapshots and the live buffer share chunks and `LinePtr`s, so versions do not duplicate unchanged lines.
- Line splits and joins are O(T + N/T). Inserting or deleting a line shifts pointers within one chunk (T ≈ 512) and
  updates the chunk index. The first edit of a chunk that a snapshot shares clones it (512 pointer copies).
- Region deletion and large pastes drop or build whole chunks. The cost depends on the size of the region, not of
  the file.
- Huge lines: an edit to a line that a snapshot shares copies the whole line string. So history memory grows by the
  size of the line per version (e.g. a 4 MB minified line).
- Rendering always redraws everything. `render()` rebuilds the whole screen string on every keystroke. This is
  enough for typical terminal sizes, but it can cause lag on very large terminals over slow connections.
- Syntax cache: cx caches the tokenisation result of each line. After an edit, it computes them again only from the
  edited line down, until the tokenizer state matches what it was before the edit. So it does not tokenise the
  whole file again on each keystroke.

A production editor for files of 100 MB and more would do better with a gap buffer, a rope or a piece table. The
current model is chosen for simplicity and readability.
