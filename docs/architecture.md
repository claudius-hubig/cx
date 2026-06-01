# cx Architecture

## Overview

`cx` is a simple terminal text editor. Its architecture is split into two
layers: a **terminal layer** that handles raw I/O and rendering, and a
**buffer layer** that owns all text content and editing state. The two
layers are decoupled through the `BufferSink` interface so that tests can
exercise the buffer layer without a real terminal. A separate **shortcut
module** provides the single source of truth for all keybindings.

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

Entry point. Constructs `SetupTerminal`, `Screen`, and `BufferManager` in
the correct order, opens any files given on the command line, then calls
`Screen::loop()` to run the editor. Each path argument is parsed via
`parseFileArg()` (`src/file_arg.hpp`) to honour an optional trailing
`:LINE` suffix (e.g. `cx src/foo.cpp:42`). If stdin is a pipe, `main`
drains it before raw-mode setup, dups `/dev/tty` over fd 0 so terminal
init still works, and seeds the captured bytes into a `*stdin*` scratch
buffer.

---

### `SetupTerminal` (`src/terminal/setup_terminal.hpp/.cpp`)

RAII wrapper around terminal configuration. On construction, saves the
current `termios` state and puts stdin into raw mode; switches the
terminal to the alternate screen buffer; enables SGR mouse tracking
(`?1000h` press/release, `?1002h` motion-while-held for drag-select,
`?1006h` extended coordinates; `?1003h` any-motion is deliberately not
enabled). On destruction, disables mouse tracking,
restores the saved state, and returns to the normal screen. Guarantees
cleanup even on exception.

---

### `Shortcut` (`src/shortcut.hpp/.cpp`)

Defines the `Action` enum (one value per editor command), the `Shortcut`
struct (modifier flags + key), and a single `constexpr` binding table
that is the **only** place keybindings are defined. Both input dispatch
and help display read from this table.

Key API:
- `handleShortcut(Shortcut)` — forward lookup: returns the `Action` for
  a shortcut, or `nullopt` if unrecognised.
- `shortcutForAction(Action)` — reverse lookup: returns the primary
  shortcut for an action (first match in the table).
- `formatShortcut(Shortcut)` — human-readable label (e.g. `"Ctrl-s"`,
  `"Esc-?"`, `"Ctrl-Bksp"`).
- `descriptionForAction(Action)` — short description string.
- `shortcutFromCtrlChar(char)` / `shortcutFromEscChar(char)` — convert
  raw terminal bytes to `Shortcut` values.

Actions: `Help`, `Exit`, `GoToLine`, `Save`, `KillBuffer`, `DeleteLine`,
`SwitchBuffer`, `ToggleMark`, `Cut`, `Copy`, `Paste`, `PasteCycle`,
`DeleteChunkBefore`, `Revert`, `VersionHistory`, `Find`,
`FindBackward`, `Replace`, `BufferList`, `PasteRing`, `ToggleWhitespace`,
`ToggleWrap`, `Format`, `ToggleComment`, `SetMode`, `DirBrowser`, `PipeReplace`,
`SplitVertical`, `SplitHorizontal`, `ClosePanel`,
`FocusPanel1`–`FocusPanel6`, `FocusMRUPanel`, `NextBlock`, `PrevBlock`,
`Recenter`, `ToggleCurrentLine`, `ExecuteCommand`, `Reflow`,
`RegexFind`, `RegexReplace`, `ToggleLineNumbers`, `ToggleMouseTracking`.

Some bindings have two shortcuts: `DeleteChunkBefore` is reachable via
both `Ctrl-Bksp` (which arrives as `Ctrl-h`) and `Esc-Bksp`. The table
contains the bare-Ctrl path first; help and reverse-lookup callers see
that one.

---

### `Screen` (`src/terminal/screen.hpp/.cpp`)

Manages all terminal I/O. Owns the main event loop and the active
`Buffer*` pointer. Implements `BufferSink` so that `BufferManager` can
push buffer switches to it.

Key responsibilities:
- **Drawing**: `draw()` calls `Buffer::render()` to produce an ANSI escape
  string, then writes it to stdout along with a status line and cursor
  positioning. Manages timed status messages and find-highlight expiry.
- **Input loop**: `loop()` reads raw keystrokes, handles SIGWINCH, and
  dispatches to sub-handlers:
  - `handlePromptInput` — generic handler for the active `Prompt`, driving
    text entry, history navigation, autocomplete, and confirmation
  - `handleEscapeInput` — CSI escape sequences (arrows, page keys),
    Esc-prefix chords, and SGR mouse payloads routed to `handleMouseEvent`
  - `handleNormalInput` — printable characters and control keys in normal
    editing mode
  - `handleMouseEvent` / `extendDragSelection` — left-press positions the
    cursor and arms a drag (a plain click keeps any existing mark);
    left-motion lazily sets the mark to the drag anchor and extends the
    `mark`→point region, autoscrolling at the top/bottom content row;
    left-release ends the drag; middle-press pastes the X11 PRIMARY
    selection at the cursor; the wheel scrolls the panel under the pointer
- **Action dispatch**: `dispatchAction(Action)` is the centralised switch
  that executes the effect of any `Action`. Three input paths feed it,
  all through `handleShortcut()`: the bare Ctrl-key path
  (`shortcutFromCtrlChar`), the Esc-chord path (`shortcutFromEscChar`),
  and the two-key C-x prefix path (`shortcutFromCxChar`, driven by
  `handleCtrlXInput()`). Unrecognised shortcuts produce a status message.
  Many actions guard at entry through `requireNotInHistoryMode()`, which
  posts a status message and returns `true` when a view is currently
  browsing history so the caller can `break` out without effect.
- **Find**: `findNext()` and `findPrev()` implement forward and backward
  search with smart matching (case-insensitive when the pattern is
  lowercase; digits and special characters in the buffer are skipped
  unless they appear in the pattern). On match, sets the cursor after the
  last matched character and applies a timed highlight.
- **Subprocess execution**: `runPipeCommand()` is the unified subprocess
  runner used by Format (Esc-f), Tab-format (Cpp mode), and PipeReplace
  (Ctrl-p). It forks/execs an external command, pipes stdin/stdout/stderr,
  and monitors the child with `poll()`. After 10 seconds of inactivity it
  displays a kill prompt; the user can press `y` to SIGTERM/SIGKILL the
  child or `n` to dismiss. SIGWINCH is handled during the poll loop so
  the terminal redraws correctly while a command is running.

`Screen` holds session-wide state that does not belong to any one
buffer: the active prompt, find state, the paste ring, the X11
`Clipboard` adapter, and the timed status message. Per-view display
toggles (soft-wrap, whitespace markers) live on `BufferView` so that
different panels can be configured independently. After every keystroke, `loop()` calls `syncPrimarySelection()`
to mirror the active mark region into the X11 PRIMARY selection (no-op
when there is no mark or no graphical session).

**Read-only overlays**: `Help`, `BufferList` and `PasteRing` switch the
focused panel to a read-only buffer while stashing the previous buffer
in `Panel::prevBuf`. `Esc-Esc`, `q`, or pressing the same overlay key a
second time pops the stash and restores the prior buffer (and, with it,
the saved cursor / mark / viewport). `handleBareEscEsc()` services the
overlay-dismissal half of `Esc-Esc`; clearing the mark only happens when
no overlay is up. `updateFocusPointers()` re-binds `_buf` / `_view` to
the focused panel after layout changes — split, close, focus, kill, or
overlay restoration.

**Helpers used across action handlers**: `bufferWorkDir()` returns the
parent directory of the focused buffer's file (or the process working
directory for pathless buffers) and is the working directory for every
external command launched via `runPipeCommand()`.

When opening a file with non-Unix line endings, `checkLineEndings()`
inspects the `LineEndingStyle` set during `TextBuffer::loadFromFile()`
and either prompts the user to translate (CRLF / lone CR) or surfaces
a "mixed line endings" status message. The flag is acknowledged on
read, so the same buffer is only flagged once per load.

Prompts are represented by the `Prompt` struct (`src/prompt.hpp`), which
carries a label, callbacks for autocomplete/result/abort/tab/special keys,
and cached display state. Each prompt type is constructed by an
`openXxxPrompt()` helper (e.g. `openGoToLinePrompt()`,
`openSwitchBufferPrompt()`, `openConfirmPrompt()`). `PromptHistory`
(also in `prompt.hpp`) records per-label input history, navigable with
Up/Down arrows.

---

### `BufferSink` (`src/buffer/buffer_sink.hpp`)

Abstract interface with a single method `setBuffer(Buffer&)`. Decouples
`BufferManager` from `Screen` so that unit tests can supply a stub sink
without constructing a real terminal.

---

### `BufferManager` (`src/buffer/buffer_manager.hpp/.cpp`)

Owns all `TextBuffer` instances (keyed by canonical path or synthetic
name), all `DirBuffer` instances (keyed by canonical directory path),
plus the singleton `HelpBuffer`, and lazily-created `BufferListBuffer`
and `PasteRingBuffer`. Maintains an MRU list for buffer switching.

Key responsibilities:
- `open()` — canonicalizes a path and opens or returns an existing
  `TextBuffer`
- `openDir()` — canonicalizes a directory path and opens or returns an
  existing `DirBuffer`
- `create()` — creates a new empty `TextBuffer` with a synthetic name
- `kill()` — removes a buffer; always leaves at least one buffer alive
- `completions()` — returns match candidates for the switch-buffer prompt,
  covering both open buffers and filesystem entries
- `displayName()` — returns a short unambiguous name for status line and
  buffer-list display (filename only when unambiguous, full path otherwise)
- `touch()` / `secondMRU()` — MRU management for default buffer switching
- `rename()` — re-keys a buffer in the map after save-as

---

### `Buffer` (`src/buffer/buffer.hpp/.cpp`)

Abstract base for all buffer types. Owns *document content only* — line
storage, filename, mode, modified flag, document version, and the
per-line syntax cache. All *view state* (cursor position, viewport,
mark, selection highlight, per-view caches) lives in `BufferView`, so
multiple views can share the same `Buffer` for split-window editing.

Text is stored as `std::vector<LinePtr>` where `LinePtr` is
`std::shared_ptr<std::string const>` (one immutable string per line,
without newline delimiters). The `LinePtr`s are shared with
`VersionHistory` snapshots so capturing or restoring a snapshot is
O(N pointer copies), not O(N string copies). Mutations go through
`Buffer::mutLine(idx)` which detaches a fresh string copy when
the existing `LinePtr` is shared (use_count > 1) and otherwise
mutates in place via `const_cast`. An empty buffer has a single
empty-string `LinePtr`; a file ending in `\n` is represented by an
empty trailing entry.

Public API:

- **Position-explicit editing**: `insertAt(cp, point)`, `deleteAt(point)`,
  `deleteBefore(point)`, `insertText(text, point)`, `deleteRegion(from, to)`,
  `extractRegion(from, to)`. Every editing method takes the cursor position
  as a parameter and returns the new position — `Buffer` never reads or
  writes any view state.
- **Rendering**: virtual `render(out, rows, cols, RenderParams)` produces
  an ANSI escape string for the visible portion of the buffer, including
  syntax highlighting (via per-mode tokenizers), region highlighting
  (inverse video when a selection mark is active), and find highlighting.
  `RenderParams` bundles the view-state inputs (point, viewFrom, mark,
  highlight, showWhitespace, wrap, row/col offsets). Returns the
  screen `{row, col}` of the cursor.
- **Screen row query**: virtual `screenRow(cols, point, viewFrom, wrap)`
  reports where a given point sits relative to a viewport start, used
  by `BufferView::ensureVisible` and similar.
- **Mode**: `BufferMode` enum (`Txt`, `Cpp`, `Md`, `Jsn`, `Dif`, `Sh`,
  `Yml`, `Py`) determines syntax highlighting, auto-indent, and
  formatting behaviour.
  Changed via `setMode()`, which invalidates the per-line syntax cache.
- **Line-number gutter**: `RenderParams::leftGutter` (in screen columns)
  reserves space at the left of the panel for right-aligned 1-based
  physical-line numbers. `Buffer::render` honours it; subclassed
  buffers (Help / Dir / BufferList / PasteRing) opt out via
  `supportsLineNumberGutter()` so a per-view toggle silently no-ops on
  overlays. Per-view state (`BufferView::showLineNumbers`,
  `lineNumberGutterWidth`) is the source of truth for the toggle.
- **Document version**: `docVersion()` is incremented on every edit so
  `BufferView` can detect external modifications made by another view
  sharing the buffer and invoke `clampToBuffer()` to re-validate its
  cached positions.
- **Saved view state**: for buffer switching, a `SavedViewState`
  ({point, viewFrom, mark}) can be stashed on the buffer via
  `setSavedView()` / `savedView()` / `clearSavedView()` and later
  restored by `BufferView::fromSavedState()`.
- **Version-history hooks**: `recordVersion(cursor, dirtyFrom, lineShift)`
  is invoked by `BufferView` after every successful edit; the default
  is a no-op (read-only buffers ignore it). `TextBuffer` overrides it to
  append a snapshot to its `VersionHistory`. The history-browsing
  interface (`browsingHistory()`, `setBrowsingHistory()`,
  `versionHistoryInfo()`, `applyVersionAt()`, `truncateHistoryAfter()`)
  is virtual on the base class so `BufferView` can drive history
  navigation through the abstract type without depending on
  `TextBuffer`.

Editing methods are guarded by `readonly()` (a pure virtual that derived
classes override). The `_modified` flag is set on any edit and cleared by
a successful `save()`. While history browsing is active, `TextBuffer`
reports `readonly() == true`, so all editing entry points become no-ops
without any per-method special-casing.

Caching:
- `_synCache` — per-line syntax tokenisation cache, invalidated on edits.
  Exposed to other subsystems via `cachedLineTokens(line)` so e.g.
  bracket-match scans reuse the same tokens that the renderer already
  computed.
- `_bracketCache` — last `findMatchingBracket()` result, keyed by point
  and `docVersion`. Lets repeated render passes reuse a previously
  computed match without rescanning.

Conflict detection:
- `scanForConflicts()` walks the line vector setting `_hasConflicts` if
  any line is a 7-character conflict marker (`<<<<<<<`, `=======`,
  `>>>>>>>`). Triggered by load, revert, and save in `TextBuffer`. When
  set, the syntax-cache pass overlays each conflict-marker line with a
  `ConflictMarker` token regardless of mode, so even a `Txt` buffer
  highlights merge regions with distinct backgrounds.

### `BufferView` (`src/buffer/buffer_view.hpp/.cpp`)

Owns all view-specific state for one logical "view" of a `Buffer`:
point (`BufferPoint`), mark, viewport (`ViewFrom`), wrap mode, desired
column for vertical movement, find highlight, and per-view caches
(`PointColCache`, `RowBoundsCache`). Multiple `BufferView`s can share
the same `Buffer`, each with independent cursor and viewport.

All user-facing movement, editing, and render operations are methods on
`BufferView`:

- **Movement**: `moveLeft/Right/Up/Down`, `moveHome/End`,
  `moveWordLeft/Right`, `moveParagraphUp/Down`, `movePhysicalUp/Down`,
  `pageUp/Down`, `gotoLine`, `gotoMatchingBracket`, `setPoint`.
- **Editing**: `insert`, `deleteAtPoint`, `deleteBeforePoint`,
  `deleteChunkAtPoint`, `deleteChunkBeforePoint`, `insertText`,
  `deleteRegion`. These call the position-explicit methods on `Buffer`
  and apply the necessary mark-adjustment locally.
- **Mark and region**: `toggleMark`, `setMark`, `clearMark`,
  `extractRegion`.
- **Viewport management**: `ensureVisible`, `scrollUp/Down`,
  `scrollViewNearEnd`, `currentLineWraps`. The viewport is described
  by `ViewFrom` which holds a `RowIdx line` and a mode-dependent
  sub-position: `ByteOffset` in wrap mode (sub-line scroll), or
  `VisualCol` in no-wrap mode (horizontal scroll).
- **Rendering**: `render()` builds a `RenderParams` from the view's
  state and forwards to `Buffer::render()`.
- **Highlight**: `setHighlight/clearHighlight` for find highlighting.
- **Cursor helpers**: `adjustCursorToIndent()` advances the cursor to
  the first non-space byte when it is in the leading whitespace.
- **History mode**: `enterHistoryMode()`, `stepHistory(delta)`, and
  `exitHistoryModeCommit()` provide read-only browsing of the
  underlying buffer's version history. Entering flips the buffer to
  read-only via `setBrowsingHistory(true)`; stepping calls
  `applyVersionAt()` and updates the cursor; committing truncates any
  forward history beyond the selected version and re-enables editing.
  `inHistoryMode()` / `historyIndex()` expose the current state to
  `Screen` for status-line rendering.
- **Consistency**: `clampToBuffer()` re-clamps point, mark, and
  viewport to valid positions after the underlying buffer changed
  (e.g. via another view or `revert()`). Called automatically when the
  view notices a `docVersion` mismatch.

---

### `TextBuffer` (`src/buffer/text_buffer.hpp/.cpp`)

Concrete editable buffer backed by a file on disk. Reads the file on
construction; `save()` writes lines back as a truncating binary write
and returns `std::expected<void, std::string>` with an error message on
failure. `revert()` reloads the buffer contents from its backing file
and bumps `docVersion`; it does *not* touch view state. Callers that
need to reposition the cursor after a revert (e.g. `Screen` on the
user-invoked `Revert` action) explicitly reset `_view->setPoint(0,0)`
and clear the mark. Exposes `setPath()` for the save-as rename
workflow. `detectMode()` sets the `BufferMode` based on file extension
(`.c`, `.h`, `.cpp`, `.hpp` → `Cpp`; `.md` → `Md`; `.json` → `Jsn`;
`.diff`, `.patch` → `Dif`; `.sh`, `.bash`, `.zsh`, `.ksh` → `Sh`;
`.yml`, `.yaml` → `Yml`; `.py`, `.pyi`, `.pyw` → `Py`; otherwise
`Txt`). When the extension is unknown, a leading `#!` line is sniffed
for an interpreter (`bash`/`zsh`/`ksh`/`dash`/`sh` → `Sh`, `python`
→ `Py`) as a fallback; only line 0 is inspected, so `detectMode()`
now runs after `loadFromFile()` in the file constructor.

`TextBuffer` also tracks the on-disk mtime + size (captured on load and
after a successful `save()`) so `changedOnDisk()` can warn when the file
has been modified by another process. `Screen::Save` checks that flag
before overwriting and prompts the user when it is set.

Line-ending style is captured during `loadFromFile()` into a
`LineEndingStyle` enum (`Unix`, `Crlf`, `Cr`, `Mixed`). `\r` bytes are
left intact in the line strings so they remain editable; `Screen` reads
the flag via `lineEndingStyle()`, prompts the user to normalize on
non-Unix files, and calls `normalizeLineEndings()` on consent
(`acknowledgeLineEndingStyle()` clears the flag so the same load isn't
flagged twice). `Mixed` files are surfaced as a status warning rather
than prompted — they cannot be unambiguously translated.

`TextBuffer` owns a `VersionHistory _history` and a
`bool _browsingHistory` flag. The history is captured on construction,
on every successful edit (via the inherited `recordVersion()` hook),
and on `save()` / `revert()` to preserve the on-disk state as a
restorable snapshot. `readonly()` returns `_browsingHistory`, which
makes the buffer immutable while a view is stepping through history.

---

### `HelpBuffer` (`src/buffer/help_buffer.hpp/.cpp`)

Readonly buffer that renders the key-binding reference. Overrides
`render()` and `screenRow()` to lay out command and movement sections
side-by-side when the terminal is wide enough, or stacked otherwise. The
command section is generated from the shortcut binding table via
`formatShortcut()` and `descriptionForAction()`, ensuring the help
display stays in sync with actual keybindings.

---

### `BufferListBuffer` (`src/buffer/buffer_list_buffer.hpp/.cpp`)

Readonly buffer that displays open buffers as a trie-based directory tree
with collapsed single-child nodes. Overrides `render()` to highlight the
selected line with reverse video. `selectedName()` returns the buffer name
under the cursor for use by `Screen` when the user presses Enter.

---

### `DirBuffer` (`src/buffer/dir_buffer.hpp/.cpp`)

Readonly buffer that displays a directory listing with permissions,
owner, group, size, and modification time. Supports:
- Navigation into subdirectories and parent (`.` and `..` entries)
- Toggling hidden (dotfile) visibility
- Marking entries for deletion and executing bulk delete
- Renaming the selected entry (`r`) and creating subdirectories (`m`)

Each entry is represented as a `DirBuffer::Entry` struct with metadata
gathered from the filesystem. `refresh()` rebuilds the listing from disk.

---

### `PasteRing` (`src/paste_ring.hpp`)

Fixed-capacity ring buffer for cut/copied text. Stores entries as
`std::string` with embedded `'\n'` for line breaks. Index 0 is the most
recent entry. Used by `Screen` to implement cut, copy, paste, and paste
cycling.

---

### `Clipboard` (`src/clipboard.hpp/.cpp`)

Best-effort X11 clipboard integration. Detects `DISPLAY` and the
presence of `xclip` (preferred) or `xsel` on `$PATH` lazily on first
use; if neither is available, all methods become silent no-ops so the
editor still works on a tty without a graphical session. Project rules
forbid linking `libX11`, so all interaction is via these external
helpers.

`Screen` calls `setPrimary()` after every keystroke (in `loop()`,
through `syncPrimarySelection()`) so the active mark region tracks the
X11 PRIMARY selection — the buffer that mouse-drag selections populate
and middle-click pastes. Internal dedup against the last published
text means cursor moves that don't change the selected text cost zero
forks. `Screen` also calls `setClipboard()` on cut and copy so the
text becomes available to other apps via the standard Ctrl-C/Ctrl-V
clipboard, `getClipboard()` on paste so external copies flow into the
editor (and onto the paste ring), and `getPrimary()` on middle-click so
the PRIMARY selection pastes back in at the cursor. `getClipboard()`
and `getPrimary()` share a private `getSelection()` helper.

Writes use a double-fork detach: the parent forks an intermediate
child, which forks again and exits, leaving the grandchild xclip/xsel
to be reaped by init. This keeps the input loop responsive (~1 ms per
write) regardless of xclip startup latency. Reads are synchronous
because the caller (Paste) needs the result immediately.

Errors are logged to `~/.cx-errors` via `error_log` and never surfaced
to the user.

---

### `PasteRingBuffer` (`src/buffer/paste_ring_buffer.hpp/.cpp`)

Readonly buffer that displays the paste ring contents. Similar pattern to
`BufferListBuffer` — `refresh()` rebuilds lines from the current ring
state.

---

### `VersionHistory` (`src/buffer/version_history.hpp/.cpp`)

Append-only history of buffer snapshots used by `TextBuffer` to
implement undo and version browsing. Each `VersionSnapshot` records
the buffer's lines at the time of capture (as a `vector<LinePtr>` of
`shared_ptr<string const>`), a timestamp, and the cursor position.

To keep capture cost low for the common case — single-line edits — a
snapshot may instead store a `vector<LinePatch>` describing only the
lines that changed since its predecessor. These delta snapshots are
materialised lazily on `at(idx)`. `kMaxDeltaChain` bounds the longest
chain of consecutive deltas before a full snapshot is forced; this
keeps materialisation O(chain length) rather than O(history size).

Pruning runs whenever `_versions.size()` exceeds `kMaxVersions` and
trims the oldest entries down to `kThinCount`. Pruning is also
delta-aware: dropped entries' patches are merged forward into their
successors instead of materialising full snapshots first, so prune
cost is proportional to the number of full snapshots dropped (a small
constant) rather than to the total history size.

Public surface: `capture()`, `captureInitial()` (initial snapshot,
moves the buffer's `lines` into a single shared block to avoid N
individual `make_shared` allocations), `at(idx)`, `truncateAfter(idx)`
(for committing a history-mode rewind), `size()` / `empty()` /
`clear()`.

---

### `Panel` (`src/terminal/panel.hpp/.cpp`)

A panel is one pane of the terminal. It owns a `BufferView`, an integer
id, a `PanelRect` describing its absolute position and size, and a
`Buffer* prevBuf` slot used by the read-only overlay flow (see
`Screen` above). Panels provide `setBuffer()` to switch to a different
buffer (optionally restoring saved view state via
`BufferView::fromSavedState()`), and `saveViewState()` to persist
cursor / mark / viewport / wrap state before the view is destroyed.
`saveViewState()` is called whenever a panel is about to drop its
`BufferView` — buffer switch or panel close — not just at last-view
destruction.

---

### `PanelLayout` (`src/terminal/panel_layout.hpp/.cpp`)

Binary split tree managing a set of panels. Each leaf owns a `Panel`;
internal nodes record a split direction (`Vertical` / `Horizontal`).
The tree is recomputed on `resize()`, `splitFocused()`, and
`closeFocused()` to assign each panel an absolute `PanelRect`.

Key operations:
- `splitFocused(dir, buf)` — replace the focused leaf with a split
  node; the original panel stays left/top and a new panel is created
  right/bottom. Returns `nullptr` if the split would make panels
  smaller than the minimum dimensions (10 cols, 3 rows). Panels are
  numbered from 1 upward; `FocusPanel1`..`FocusPanel6` correspond to
  the first six panels in tree order, which is the practical maximum.
- `closeFocused()` — remove the focused leaf and promote its sibling.
  Restores focus to the most recently used panel via an MRU history,
  falling back to the first panel in tree order.
- `focusPanel(id)` — switch focus by panel id; updates the MRU history.
- `forEachPanel(fn)` — visit every panel in tree order.

---

### `BufferSink` / `BufferManager` interaction

`BufferManager` holds a `BufferSink&` and calls `setBuffer()` on it
whenever a new buffer is switched to (from `open`, `openDir`, `create`,
the constructor's initial buffer, and `kill`'s replacement). `Screen`
implements `BufferSink::setBuffer()` by updating its `_buf` pointer and
calling `draw()`. This one-way dependency means `BufferManager` never
knows about `Screen` concretely.

---

## Syntax Highlighting

Syntax highlighting is mode-driven and lives in `src/buffer/modes/`:

- **C/C++ mode** (`modes/c/syntax.hpp/.cpp`): Tokenizes keywords,
  comments (line and block), strings (regular, character literals, raw
  strings with `R"delim(...)delim"`), value literals (`Number`:
  numeric literals — integer/float, `0x`/`0b` prefixes, `'` digit
  separators, hex floats, `u`/`l`/`f`/`z`/user-defined suffixes — plus
  the boolean/null literals `true`/`false`/`nullptr`, which are values
  rather than keywords and so share the numeric colour), operators, and
  brackets (parens, brackets, braces, angles). Maintains multi-line state
  (`SyntaxState`) for block comments and raw string literals. Also
  provides `findMatchingBracket()` for jump-to-matching-bracket
  support.

- **Markdown mode** (`modes/md/syntax.hpp/.cpp`): Tokenizes headings
  (`# ...`), bold (`**...**`, single-line), and links (`[text](url)`).
  Links may span lines: `state.mdLink` (`MdLink::Label`/`Url`) carries
  an open `[...` label or `](...` destination across the boundary and
  is Link-coloured until it closes (optimistically to end of file if
  it never does). Headings are suppressed while a link continuation is
  open. Bold is single-line only. Inline code spans (`` `...` ``) are
  skipped so markup inside them is not tokenised — most importantly a
  stray `[` cannot open a link, which prevents an unbalanced bracket in
  code from carrying link highlighting onto the following lines.

- **Diff mode** (`modes/dif/syntax.hpp/.cpp`): Classifies entire lines
  based on their prefix: `+` (add), `-` (remove), `+++`/`---` (file
  headers), `@@` (hunk headers). No multi-line state needed.

- **JSON mode** (`modes/jsn/syntax.hpp/.cpp`): Tokenizes strings,
  numbers, the value literals `true`/`false`/`null` (coloured as
  `Number`, like numeric literals — JSON emits no `Keyword`), and
  brackets (`[]`, `{}`). No multi-line state needed (the `SyntaxState`
  parameter is accepted for interface consistency only). Provides
  `findMatchingBracketJson()` for jump-to-matching-bracket; only `[]`
  and `{}` are matched, brackets inside strings are skipped.

- **Shell mode** (`modes/sh/syntax.hpp/.cpp`): Tokenizes `#` comments,
  single/double-quoted and backtick strings, the reserved word set
  (`if`/`then`/`fi`/`for`/`while`/`do`/`done`/…), `$VAR`/`${...}`/`$?`
  parameter expansions (the `Variable` token), brackets, and the `|`
  `&` `;` `<` `>` `=` operators. Maintains multi-line state for
  heredocs: `state.heredocTag` makes every following line one `String`
  span until a line equal to the tag (leading tabs stripped for the
  `<<-` form, tracked by `state.heredocStripTabs`); `<<<` here-strings
  are not heredocs.

- **YAML mode** (`modes/yml/syntax.hpp/.cpp`): Tokenizes `#` comments,
  mapping keys (`Keyword`, the text before a `: `/EOL colon), `'...'`
  and `"..."` strings, the plain constants `true`/`false`/`yes`/`no`/
  `on`/`off`/`null`/`~`/`nan` (case-insensitive) and numeric scalars —
  both value literals, coloured as `Number`; mapping keys remain
  `Keyword` — `: , -` and flow brackets, and node properties
  `&anchor`/`*alias`/`!tag` (`Preprocessor`). Directives (`%YAML`) and
  document markers (`---`/`...`) are `Preprocessor`. Maintains
  multi-line state for `|`/`>` block scalars: `state.yamlBlockCol`
  holds the introducing line's indent; deeper-indented or blank lines
  are one `String` span until a non-blank line dedents back to it.

- **Python mode** (`modes/py/syntax.hpp/.cpp`): Tokenizes `#`
  comments, strings with optional `r`/`b`/`f`/`u` prefixes
  (single/double and triple-quoted), the keyword set
  (`def`/`class`/`for`/`lambda`/… ), numbers
  (`0x`/`0o`/`0b`, `_` groups, fraction/exponent, imaginary `j`) and
  the value literals `None`/`True`/`False` (coloured as `Number`, not
  `Keyword`),
  `@decorator` lines (`Preprocessor`), brackets, and `:` (`Operator`).
  Maintains multi-line state for triple-quoted strings:
  `state.pyTriple` holds the open quote char and `state.pyTripleRaw`
  suppresses escape handling for `r'''...'''`; f-string `{...}`
  interiors stay inside the `String` span (no recursion). Python is
  also added to the leading-indent-preserving set in
  `Buffer::insertAt` (alongside Cpp/Jsn).

`BufferView::gotoMatchingBracket()` dispatches to the JSON matcher in
JSON mode and to the C/C++ matcher in every other mode. Because the
C/C++ matcher only treats `<>` as brackets when tagged `Angle`, plain
text, Markdown, shell, YAML and Python buffers (where `<>` are
`Operator`/`Normal`, never `Angle`) get a clean `()[]{}`-only
fallback for free.

- **Conflict markers**: Regardless of mode, lines matching `<<<<<<<`,
  `=======`, or `>>>>>>>` (7 identical characters) are overlaid with
  `ConflictMarker` tokens when `_hasConflicts` is set. The flag is
  computed on load, revert, and save via `scanForConflicts()`.

The `SyntaxToken` enum (`Normal`, `Keyword`, `Comment`, `String`,
`Operator`, `Paren`, `Bracket`, `Brace`, `Angle`, `Heading`, `Bold`,
`Link`, `Number`, `Preprocessor`, `DiffAdd`, `DiffRemove`, `DiffMeta`,
`DiffHunk`, `ConflictMarker`, `Variable`) is shared across modes.
Tokens are mapped to ANSI colours in `Buffer::render()`.

Per-line tokenisation results are cached in `Buffer::_synCache` and
invalidated from the edited line forward. `Buffer::render()` extends
the cache up to just past the last visible line via `ensureSynCache()`;
`Buffer::cachedLineTokens()` exposes this incrementally-extended cache
to `findMatchingBracket()`, so jump-to-matching-bracket looks up
pre-computed tokens for any line in the visible window and only
tokenises on demand when a forward scan runs off the bottom.

The cheaper `advanceBlockCommentState()` exists for callers that only
need to maintain the multi-line block-comment / raw-string-literal
state without producing tokens. Unlike `tokenizeLine()` it skips
identifier/keyword classification, bracket tagging, and the per-byte
token vector allocation.

---

### Supporting types and utilities

| File                                   | Contents                                                                                                                     |
|----------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `src/types.hpp`                        | `RowIdx`, `ColIdx`, `contentWidth()`, `BufferPoint`, `ByteOffset`, `VisualCol`, `ViewFrom`                                   |
| `src/byte_constants.hpp`               | Named constants for terminal input bytes (`key::` namespace) and ANSI output sequences (`ansi::` namespace incl. `cursorTo()`) |
| `src/utf8.hpp/.cpp`                    | UTF-8 decoding/encoding (`decodeUtf8`, `encodeUtf8`), character width (`charWidth`, `codepointWidth`, `charInfoAt`), `visualWidth`, `escapeNonPrintable`, `isWhitespace`, `readUtf8Tail`, `readUtf8Codepoint` |
| `src/prompt.hpp`                       | `Prompt` struct (label, callbacks, display cache), `PromptDisplay`, `PromptHistory` (per-label input history)                 |
| `src/paste_ring.hpp`                   | `PasteRing` — fixed-capacity ring buffer for clipboard history                                                               |
| `src/clipboard.hpp/.cpp`               | X11 PRIMARY/CLIPBOARD integration via `xclip`/`xsel` subprocesses; lazy detection, async writes, sync reads                   |
| `src/error_log.hpp/.cpp`               | Appends diagnostic messages to `~/.cx-errors` for post-mortem inspection                                                     |
| `src/search.hpp`                       | Smart-find helpers (`matchAt`, `FindFlags`, `findFirstMatch`, `findLastMatch`) used by `Screen::findNext/findPrev`            |
| `src/replace.hpp`                      | Find-and-replace helpers (`findNextMatchInBuffer`, `replaceAllForward`) used by `Screen::openReplacePrompt`                  |
| `src/regex_search.hpp`                 | Regex search/replace helpers (`compileRegex`, `findFirstRegex`, `findNextRegexInBuffer`, `substituteCaptures`, `replaceAllRegexForward`) used by Esc-X / Esc-Y |
| `src/mouse.hpp`                        | SGR mouse parser (`parseSgrMouse`, `MouseEvent`) — translates `ESC[<…M/m` payloads from the input loop into structured events |
| `src/file_arg.hpp`                     | `parseFileArg` — split a `path:LINE` command-line argument into a path and an optional line number                          |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup                                                                                                 |

---

### Performance characteristics

The text storage model (`vector<LinePtr>`, one immutable
`shared_ptr<string const>` per line) is simple but has known
scalability limits:

- **Per-line overhead**: each line is a separate `std::string` plus a
  `shared_ptr` control block (~32 bytes overhead + allocator metadata).
  A file with millions of short lines wastes significant memory and
  fragments the heap. Snapshots and the live buffer share `LinePtr`s,
  so unchanged lines are not duplicated across versions.
- **Line splits/joins are O(N) shifts**: inserting or deleting a
  newline calls `_lines.insert()`/`_lines.erase()`, which shifts all
  subsequent `LinePtr`s. The shifted entries are pointer-sized so the
  cost is low per element, but still proportional to file size.
- **Region deletion is O(N)**: `deleteRegion()` creates a temporary merged
  line and erases a range of `_lines`, both proportional to file size.
- **Rendering is full-redraw**: `render()` rebuilds the entire screen
  string on every keystroke. This is adequate for typical terminal sizes
  but may cause lag on very large terminals over slow connections.
- **Syntax cache**: Per-line tokenisation results are cached and only
  recomputed from the edited line forward, avoiding full-file retokenisation
  on each keystroke.

For a production editor handling 100 MB+ files, a gap buffer, rope, or
piece table would be more appropriate. The current model is chosen for
simplicity and readability.
