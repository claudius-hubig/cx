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

Entry point. Parses the command line with `cli::parse()`
(`src/cli.hpp`) into a `cli::Command` — `ShowHelp`, `ShowVersion`, or
`OpenFiles` — before touching stdin or the terminal, and handles the
first two by printing `cli::usage()` or `versionText()`
(`src/version.hpp/.cpp`) and exiting. Otherwise it constructs
`SetupTerminal`, `Screen`, and `BufferManager` in the correct order, opens any files given on the command line, then calls
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
`RegexFind`, `RegexReplace`, `ToggleLineNumbers`, `ToggleMouseTracking`,
`SetBookmark`, `JumpBookmark`, `SelectAllOccurrences`, `AddCursorDown`,
`AddCursorUp`.

Some bindings have two shortcuts: `DeleteChunkBefore` is reachable via
both `Ctrl-Bksp` (which arrives as `Ctrl-h`) and `Esc-Bksp`. The table
contains the bare-Ctrl path first; help and reverse-lookup callers see
that one.

A few actions have no table shortcut at all (an empty `Shortcut`) because
they are driven from CSI sequences in the input loop rather than by a
chord: `ToggleOverwriteMode` (Insert key) and `AddCursorDown` /
`AddCursorUp` (`Ctrl-Shift-Down/Up`, `ESC[1;6B` / `ESC[1;6A`). They still
get table rows so they stay listed in help and reachable by name through
`Esc-x`; help renders them with `formatRawLine` since `formatShortcut`
has no chord to format.

---

### `Screen` (`src/terminal/screen.hpp/.cpp`)

Manages all terminal I/O. Owns the main event loop and the active
`Buffer*` pointer. Implements `BufferSink` so that `BufferManager` can
push buffer switches to it.

Key responsibilities:
- **Drawing**: `draw()` calls `Buffer::render()` to produce an ANSI escape
  string, then writes it to stdout along with a status line and cursor
  positioning. Manages timed status messages and find-highlight expiry.
  The cursor shape is emitted per frame from the focused buffer's
  overwrite flag via DECSCUSR — steady block (`ansi::CursorBlock`) for
  insert, steady bar (`ansi::CursorBar`) for overwrite — and reset to the
  terminal default when the alternate screen is torn down.
- **Input loop**: `loop()` reads raw keystrokes, handles SIGWINCH, and
  dispatches to sub-handlers. Any `std::exception` escaping a handler is
  logged and shown in the status line rather than ending the session:
  - `handlePromptInput` — generic handler for the active `Prompt`, driving
    text entry, history navigation, autocomplete, and confirmation;
    `handlePromptCsi` reads whole CSI sequences so unhandled ones never
    leak their tail bytes into the input
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
  the terminal redraws correctly while a command is running. A pipe that
  reaches EOF leaves the poll set, and once every pipe is closed the loop
  polls for the child's exit instead of blocking in `waitpid()`, so a
  command that closes its outputs and keeps running (`cmd 2>/dev/null`)
  neither spins the editor nor escapes the kill prompt. An optional
  `PipeLimit` (lines and bytes of stdout) stops a command early: it is
  killed, stdout is cut after the last whole line within the limit and the
  result is marked `truncated`. Grep uses 100,000 lines / 64 MB.

`Screen` holds session-wide state that does not belong to any one
buffer: the active prompt, find state, the paste ring, the X11
`Clipboard` adapter, the timed status message, and the `Bookmarks`
store (`_bookmarks`, `src/bookmarks.hpp`). Bookmarks are global across
buffers and session-only, so `Screen` — not any `Buffer` or
`BufferView`, which are per-buffer or transient — is their only
sensible owner. `openSetBookmarkPrompt()` / `openJumpBookmarkPrompt()`
(bound to `C-x b` / `C-x j`) prompt for a single slot character and
call `jumpToBookmark()`, which switches the focused panel's buffer if
needed and clamps the stored point to the target buffer's current
contents. `renameBufferAndRekeyBookmarks()` wraps every
`BufferManager::rename()` call site (both Save-As paths) so a renamed
buffer's bookmarks follow it under the new name. Buffers the manager
cannot resolve by name — the singleton `*help*`, `*buffers*` and
`*paste ring*` overlays — are refused at set time, since a bookmark
into one could never be honoured. Per-view display
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
cached display state, and a UTF-8-aware edit cursor with single-line
editing operations (insert, character/chunk deletion, character/word/
Home/End movement), plus a mark whose region `Screen::handlePromptAction`
cuts, copies and pastes through the shared paste ring (the same
shortcut-table actions as in the buffer). Callbacks that replace `input`
wholesale (Tab, Right-arrow completion, history, special keys) call
`inputReplaced()`, leaving the cursor at the end and dropping the mark.
`drawModeline()` draws the cell under the cursor in normal video against
the reverse-video bar, the region in underlined normal video, and returns
the cursor column; `draw()` parks the
terminal cursor there hidden, since on top of that cell it would invert
it back into the bar's colours. Each prompt type is constructed by an
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
plus the singleton `HelpBuffer`, and lazily-created `BufferListBuffer`,
`PasteRingBuffer`, and `GrepBuffer`. Maintains an MRU list for buffer
switching.

Key responsibilities:
- `open()` — canonicalizes a path and opens or returns an existing
  `TextBuffer`
- `openDir()` — canonicalizes a directory path and opens or returns an
  existing `DirBuffer`
- `grepBuffer()` — creates or refreshes the singleton `GrepBuffer` with a
  new result set
- `create()` — creates a new empty `TextBuffer` with a synthetic name
- `kill()` — removes a buffer; always leaves at least one buffer alive
- `completions()` — returns match candidates for the switch-buffer prompt,
  covering both open buffers and filesystem entries
- `displayName()` — returns a short unambiguous name for status line and
  buffer-list display (filename only when unambiguous, full path otherwise)
- `touch()` / `secondMRU()` — MRU management for default buffer switching
- `rename()` — re-keys a buffer in the map after save-as; canonicalises the new
  path like `open()` and refuses (returns false) if another buffer already has it

---

### `Buffer` (`src/buffer/buffer.hpp/.cpp`)

Abstract base for all buffer types. Owns *document content only* — line
storage, filename, mode, modified flag, document version, and the
per-line syntax cache. All *view state* (cursor position, viewport,
mark, selection highlight, per-view caches) lives in `BufferView`, so
multiple views can share the same `Buffer` for split-window editing.

Text is stored in a `LineStore` (see below) of `LinePtr`s, where
`LinePtr` is `std::shared_ptr<std::string const>` (one string per line,
without newline delimiters). The store's chunks and lines are shared
with `VersionHistory` snapshots, so capturing or restoring a snapshot
copies one pointer per chunk (O(N/512)), not one per line and never a
string. Mutations go through `Buffer::mutLine(idx)` →
`LineStore::mutableLine`, which detaches a shared chunk and then a
shared string before handing out a mutable reference; new lines come
from `makeLine()`. `lineContent(idx)` and `lineCount()` keep their
signatures; `lines()` returns `LineStore const&`. An empty buffer has a
single empty-string line; a file ending in `\n` is represented by an
empty trailing entry. Loading collects the lines into a vector and
moves it into the store, which keeps the line strings contiguous.

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
  highlight, extraCursors, showWhitespace, wrap, row/col offsets).
  Returns the screen `{row, col}` of the cursor.
  `RenderParams::extraCursors` is a sorted span of secondary cursor
  positions (empty in single-cursor editing); each one that falls in the
  viewport reverse-videos its character cell — a space at end-of-line —
  while the hardware terminal cursor stays on `point`.
- **Screen row query**: virtual `screenRow(cols, point, viewFrom, wrap)`
  reports where a given point sits relative to a viewport start, used
  by `BufferView::ensureVisible` and similar.
- **Mode**: `BufferMode` enum (`Txt`, `Cpp`, `Md`, `Jsn`, `Dif`, `Sh`,
  `Yml`, `Tml`, `Py`) determines syntax highlighting, auto-indent, and
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
- **Version-history hooks**: `recordVersion(cursor)` is invoked by
  `BufferView` after every successful edit (and by the replace-all
  helpers, which edit the buffer directly); the default is a no-op
  (read-only buffers ignore it). `TextBuffer` overrides it to append a
  snapshot of the whole buffer to its `VersionHistory`; the snapshot
  shares the line store's chunks, so callers need not describe what
  changed. The history-browsing
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

Insert vs. overwrite is a per-buffer flag (`_overwriteMode`, toggled via
`toggleOverwriteMode()`) that lives on the base `Buffer` so every view of
the buffer shares it. It changes only editing semantics —
`BufferView::insert()` deletes the character under the cursor before
inserting when the flag is set — plus two purely presentational cues: the
`I`/`O` modeline indicator and the terminal cursor shape (see Screen).
`drawModeline` prefixes that indicator with the view's live cursor count
as `<count>@` (blank with one cursor, `***@` above 999). The count and the
`@` occupy a fixed-width field so the `I`/`O` never changes column as
cursors are added or dropped.

Caching:
- `_synCache` — per-line syntax tokenisation cache, invalidated on edits.
  Exposed to other subsystems via `cachedLineTokens(line)` so e.g.
  bracket-match scans reuse the same tokens that the renderer already
  computed.
- `_bracketCache` — last `findMatchingBracket()` result, keyed by point
  and `docVersion`. Lets repeated render passes reuse a previously
  computed match without rescanning.

Conflict detection:
- `scanForConflicts()` walks the lines setting `_hasConflicts` if
  any line is a 7-character conflict marker (`<<<<<<<`, `=======`,
  `>>>>>>>`). Triggered by load, revert, and save in `TextBuffer`. When
  set, the syntax-cache pass overlays each conflict-marker line with a
  `ConflictMarker` token regardless of mode, so even a `Txt` buffer
  highlights merge regions with distinct backgrounds.

### `BufferView` (`src/buffer/buffer_view.hpp/.cpp`)

Owns all view-specific state for one logical "view" of a `Buffer`:
point (`BufferPoint`), mark, secondary cursors, viewport (`ViewFrom`),
wrap mode, desired column for vertical movement, find highlight, and
per-view caches (`PointColCache`, `RowBoundsCache`). Multiple
`BufferView`s can share the same `Buffer`, each with independent cursor
and viewport.

All user-facing movement, editing, and render operations are methods on
`BufferView`:

- **Movement**: `moveLeft/Right/Up/Down`, `moveHome/End`,
  `moveWordLeft/Right`, `moveParagraphUp/Down`, `movePhysicalUp/Down`,
  `pageUp/Down`, `gotoLine`, `gotoMatchingBracket`, `setPoint`.
- **Editing**: `insert`, `deleteAtPoint`, `deleteBeforePoint`,
  `deleteChunkAtPoint`, `deleteChunkBeforePoint`, `insertText`,
  `deleteRegion`. These call the position-explicit methods on `Buffer`
  and apply the necessary mark-adjustment locally. A chunk delete finds
  its span first and removes it as one edit (`deleteSpan`), so it is a
  single history entry however long the chunk is.
- **Case conversion**: `convertCase(CaseOp)` upper/lower/capitalizes the
  active region, or the word at/after the cursor when no mark is set. The
  transform is byte-wise ASCII (UTF-8-safe and length-preserving), applied
  as a single `deleteRegion`+`insertText` edit.
- **Reflow**: `reflowParagraph(width, ReflowTables)` refills the paragraph
  at the cursor, or every paragraph in the region, keeping list bullets,
  common indentation and blank lines. With `ReflowTables::Markdown` (passed
  by `Screen` for `md` buffers) GFM tables are formatted through
  `modes/md/table.hpp` instead, and fenced code blocks
  (`modes/md/fence.hpp`) are left untouched. The cursor's table is aligned and the
  cursor's column wrapped. Paragraphs stop at adjacent tables. A region
  widens to cover whole tables, which are aligned only. The result is one
  `deleteRegion`+`insertText` edit (one undo step) over just the span of
  lines that differ, so unchanged lines keep their storage (shared with
  earlier history snapshots); it is skipped when the text is unchanged.
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
- **History mode**: `enterHistoryMode()`, `stepHistory(delta)`,
  `exitHistoryModeCommit()` and `exitHistoryModeCancel()` provide
  read-only browsing of the underlying buffer's version history.
  Entering flips the buffer to read-only via
  `setBrowsingHistory(true)`; stepping calls `applyVersionAt()` and
  updates the cursor; committing (Ctrl-u, Enter) truncates any forward
  history beyond the selected version and re-enables editing;
  cancelling (Esc-Esc) shows the newest version again, puts the cursor
  back where it was on entry and drops nothing. The read-only flag
  lives on the buffer, so a view must leave history mode before it is
  dropped.
  Only one view per buffer may browse at a time: `enterHistoryMode()`
  returns false while the buffer is already browsing, since the shown
  snapshot is per buffer. `inHistoryMode()` / `historyIndex()` expose the current state to
  `Screen` for status-line rendering.
- **Multiple cursors**: `_extraCursors` holds the secondary cursors,
  kept sorted, deduplicated, and never equal to `_point`;
  `multiCursor()` *is* "that vector is non-empty", so there is no
  separate mode flag to drift. Created by `selectAllOccurrences()`
  (`Esc-*`) and `addCursorDown/Up()` (`Ctrl-Shift-Down/Up`), dropped by
  `collapseCursors()`.
  - *Editing*: `insert`, the deletes, the chunk deletes, and
    `insertText` route to `multi*` drivers. Each snapshots every cursor
    into `_batch` (sorted), visits the cursors in ascending order and
    performs primitive edits through `Buffer`'s position-explicit API.
    Since every edit happens at or after the visited cursor, the
    unvisited tail is shifted lazily: `BatchShift` (an anchor pair: a
    pre-batch position and where it is now) maps each cursor once, in
    `batchVisit()`, which also detects cursors merged onto their
    predecessor. Only a backward deletion can reach visited cursors; it
    collapses them onto its start. A keystroke is O(N) in the cursor
    count.
  - *History*: the batch ends with exactly one `recordVersion(point)`,
    so one keystroke is one undo step regardless of cursor count.
  - *Movement*: the parallel commands are thin wrappers whose `*Impl`
    body runs once for the primary and then once per extra cursor
    (`moveExtras()`), each extra temporarily installed as `_point`.
    Commands that jump somewhere unrelated — `setPoint` (and so find,
    bookmarks, mouse clicks), `gotoLine`, paging, paragraph moves,
    bracket match — collapse to the primary first, as do setting the
    mark, region edits, and entering history mode. Conversely, adding a
    cursor clears the mark (selections are not per-cursor), and so does
    every batch edit, since batch edits never shift it.
  - Extra cursors are deliberately **not** persisted in
    `Buffer::SavedViewState`: a buffer switch or panel close collapses
    to the primary cursor.
- **Consistency**: `clampToBuffer()` re-clamps point, mark, extra
  cursors, and viewport to valid positions after the underlying buffer
  changed (e.g. via another view or `revert()`). Called automatically
  when the view notices a `docVersion` mismatch. Every clamped byte
  offset (and `setPoint()`) is snapped back to a code-point boundary via
  `snapToCodepoint()` (`utf8.hpp`), so a stale position can never split a
  multi-byte character. The view's own edits adopt the new version via
  `syncAfterEdit()`, which also keeps the viewport in range.

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
`.yml`, `.yaml` → `Yml`; `.toml` → `Tml`; `.py`, `.pyi`, `.pyw` → `Py`; otherwise
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
Showing the newest version again while browsing restores the
`modified()` flag the buffer had when browsing began, so stepping back
and returning (or cancelling) leaves a saved buffer unmodified.

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

### `GrepBuffer` (`src/buffer/grep_buffer.hpp/.cpp`)

Readonly buffer listing find-in-files results, one `path:line: text` row
per hit under a one-line header describing the search. `refresh()`
rebuilds the listing from a new result set; `selectedHit()` maps a cursor
position past the header back to a `GrepHit` so `Screen` can open the
file when the user presses Enter.

The free function `parseGrepOutput()` turns the raw stdout of either
backend into `GrepHit` values. Both are invoked with a NUL after the
filename (`git grep -z`, `grep -Z`), which keeps paths containing `:`
or a newline unambiguous (the path is located by its NUL before the
record's end-of-line is); they differ in whether the line number is terminated by NUL
or `:`, and the parser accepts either. Malformed records are skipped
rather than aborting the parse. Consecutive hits in one file share a
`GrepFile` (absolute and display path): a `std::filesystem::path` per
hit cost ~800 bytes, 3.4 GB for 4M hits. The display rows are built
once, straight into the base-class `LineStore`. A result set cut short by
`runGrep()`'s output limit is marked in the header ("search stopped at
the limit").

Backend selection, argument construction, and opening a hit live in
`Screen` (`runGrep()`, `searchStartDir()`, `openGrepHit()`), not here, so
this class stays a pure display of an already-computed result set:
- inside a git work tree, `git grep` runs from the repository root, so
  `.gitignore` is honoured (and only tracked files are searched)
- otherwise `grep -r` runs from `searchStartDir()` — the browsed
  directory for a `DirBuffer`, else the current file's parent, else the
  process working directory
- `openGrepHit()` shows the target in the other panel, splitting
  vertically first when only one panel is open, so the listing survives
  walking several matches

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

### `LineStore` (`src/buffer/line_store.hpp/.cpp`)

An indexed sequence of `LinePtr`s kept in shared copy-on-write chunks of
up to 2×T pointers (T = 512 by default, a constructor parameter so tests
can use tiny chunks; every chunk but the last holds at least T/4).
Copying a store copies one pointer per chunk, and the copies share every
chunk; the first edit of a shared chunk clones it, and `mutableLine(i)`
then also detaches a shared line string. Line `i` is found by binary
search over the chunks' first line numbers, with a cached last chunk
that makes sequential access O(1); edits cost O(T + N/T). Large inserts
build whole chunks and large erases drop whole chunks. The free
`makeLine()` helpers create line strings non-const behind the
`string const` pointer, which makes `mutableLine()`'s in-place edit of an
unshared line well-defined.

Assumptions (stated in the header): single-threaded (the lookup cache and
the `use_count()` sharing test), and references/iterators are valid only
until the next mutation. Bulk loads move a `std::vector<LinePtr>` in, so
the chunk arrays are allocated after the lines (8 KB arrays interleaved
with the line strings were measured to halve later scan speed).
`operator[]` caches the last chunk's range and array (one comparison
and one load for nearby reads); the iterator holds the current chunk's
element range. Used by `Buffer` for its lines and by `VersionHistory`
for full snapshots.

---

### `VersionHistory` (`src/buffer/version_history.hpp/.cpp`)

Append-only history of buffer snapshots used by `TextBuffer` to
implement undo and version browsing. Each `VersionSnapshot` records
the buffer's lines at the time of capture (a `LineStore` copy, which
shares the buffer's chunks), a timestamp, and the cursor position.

Every version is a complete snapshot: capturing copies one pointer per
chunk (O(N/512)) whatever the edit was, and versions keep only the
chunks and lines that differ between them, so memory grows with what
was edited, not with file size × versions. `linesAt(idx)` returns a copy
of the version's store (same cost) without changing the history;
`timestampAt(idx)` and `cursorAt(idx)` are O(1).

Pruning runs whenever `_versions.size()` exceeds `kMaxVersions` and
drops every other entry among the oldest `kThinCount`. Snapshots are
independent, so dropping one needs no fix-up of its neighbours; it
costs one reference-count decrement per chunk plus freeing what only
that version held.

Public surface: `capture(lines, cursor)`, `linesAt(idx)` /
`timestampAt(idx)` / `cursorAt(idx)`, `truncateAfter(idx)` (for
committing a history-mode rewind), `size()` / `empty()` / `clear()`.

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
  (`CSyntaxState`) for block comments and raw string literals. Also
  provides `findMatchingBracket()` for jump-to-matching-bracket
  support.

- **Markdown mode** (`modes/md/syntax.hpp/.cpp`): Tokenizes headings
  (`# ...`), bold (`**...**`, single-line), and links (`[text](url)`).
  Links may span lines: `MdSyntaxState::mdLink` (`MdLink::Label`/`Url`) carries
  an open `[...` label or `](...` destination across the boundary and
  is Link-coloured until it closes (optimistically to end of file if
  it never does). Headings are suppressed while a link continuation is
  open. Bold is single-line only. Inline code spans (`` `...` ``) are
  skipped so markup inside them is not tokenised — most importantly a
  stray `[` cannot open a link, which prevents an unbalanced bracket in
  code from carrying link highlighting onto the following lines.

- **Markdown tables** (`modes/md/table.hpp/.cpp`): Pure functions
  with no buffer or view dependency. `findPipeRun()` returns the run of
  lines containing a `|` around a line and the table header inside it
  (callers reuse it for every line of the run, keeping detection linear);
  `findMarkdownTable()` narrows that to the
  line range of the GFM pipe table around a line: a header, then a
  delimiter row with the same cell count, then body rows, every line
  holding an unescaped `|`. `formatMarkdownTable()` pads every cell to its
  column's `visualWidth` and resizes delimiter rows, keeping their `:`
  alignment markers. Given a cursor, it also wraps the cursor's column
  into the width left of the target by the other columns. That width is
  no narrower than the longest word, the header, or min(20, the widest
  cell). Code spans and links are never split. A GFM row is one line, so
  a wrapped cell continues in body rows whose first cell is empty.
  Column 1 and the header are never wrapped. A fully empty body row is
  a spacer and is kept. Rows may carry indentation and block-quote
  markers (`> `), which every output row gets, and the header a
  byte-order mark. The returned cursor keeps
  its logical cell and its non-blank-byte offset within that cell's
  text.

- **Markdown fences** (`modes/md/fence.hpp/.cpp`):
  `markdownFencedLines()` marks every line inside a fenced code block
  (``` or ~~~, closing run at least as long). Reflow leaves those lines
  untouched.

- **Diff mode** (`modes/dif/syntax.hpp/.cpp`): Classifies entire lines
  based on their prefix: `+` (add), `-` (remove), `+++`/`---` (file
  headers), `@@` (hunk headers). No multi-line state needed, so the
  tokenizer takes none.

- **JSON mode** (`modes/jsn/syntax.hpp/.cpp`): Tokenizes strings,
  numbers, the value literals `true`/`false`/`null` (coloured as
  `Number`, like numeric literals — JSON emits no `Keyword`), and
  brackets (`[]`, `{}`). No multi-line state needed, so the tokenizer
  takes none. Provides
  `findMatchingBracketJson()` for jump-to-matching-bracket; only `[]`
  and `{}` are matched, brackets inside strings are skipped.

- **Shell mode** (`modes/sh/syntax.hpp/.cpp`): Tokenizes `#` comments,
  single/double-quoted and backtick strings, the reserved word set
  (`if`/`then`/`fi`/`for`/`while`/`do`/`done`/…), `$VAR`/`${...}`/`$?`
  parameter expansions (the `Variable` token), brackets, and the `|`
  `&` `;` `<` `>` `=` operators. Maintains multi-line state
  (`ShSyntaxState`) for heredocs: `state.heredocTag` makes every following line one `String`
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
  multi-line state (`YmlSyntaxState`) for `|`/`>` block scalars: `state.yamlBlockCol`
  holds the introducing line's indent; deeper-indented or blank lines
  are one `String` span until a non-blank line dedents back to it.

- **TOML mode** (`modes/tml/syntax.hpp/.cpp`): Tokenizes `#` comments,
  table headers `[a.b]`/`[[a]]` (names `Preprocessor`, brackets
  `Bracket`), bare and dotted keys before `=` (`Keyword`; quoted key
  parts `String`), basic/literal strings and their `"""`/`'''`
  multi-line forms, and the value literals — booleans, integers
  (`0x`/`0o`/`0b`, `_` groups), floats (`inf`/`nan`) and date/times —
  as `Number`. Inside inline tables `{...}` keys are recognised after
  `{` and `,`. Maintains multi-line state (`TmlSyntaxState`): `state.tomlTriple` holds
  the open triple-quote char, `state.tomlDepth` counts `[`/`{` left
  open by earlier lines so lines inside a multi-line array are parsed
  as values (no header or key detection).

- **Python mode** (`modes/py/syntax.hpp/.cpp`): Tokenizes `#`
  comments, strings with optional `r`/`b`/`f`/`u` prefixes
  (single/double and triple-quoted), the keyword set
  (`def`/`class`/`for`/`lambda`/… ), numbers
  (`0x`/`0o`/`0b`, `_` groups, fraction/exponent, imaginary `j`) and
  the value literals `None`/`True`/`False` (coloured as `Number`, not
  `Keyword`),
  `@decorator` lines (`Preprocessor`), brackets, and `:` (`Operator`).
  Maintains multi-line state (`PySyntaxState`) for triple-quoted strings:
  `state.pyTriple` holds the open quote char and `state.pyTripleRaw`
  suppresses escape handling for `r'''...'''`; f-string `{...}`
  interiors stay inside the `String` span (no recursion). Python is
  also added to the leading-indent-preserving set in
  `Buffer::insertAt` (alongside Cpp/Jsn).

`BufferView::gotoMatchingBracket()` dispatches to the JSON matcher in
JSON mode and to the C/C++ matcher in every other mode. Because the
C/C++ matcher only treats `<>` as brackets when tagged `Angle`, plain
text, Markdown, shell, YAML, TOML and Python buffers (where `<>` are
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
invalidated from the edited line forward. Each entry, a `LineSyntax<S>`,
holds the line's tokens and the tokenizer state after it, where `S` is
the buffer mode's state struct (above), or `std::monostate` for modes
without one (txt, dif, jsn), which takes no space. The cache is a
`SyntaxCache` (`buffer/syntax_cache.hpp`), a `std::variant` of one entry
vector per state type: the variant is per buffer, so entries carry no
tag and each is only as large as its own mode needs (48 bytes for C and
shell, 32 for Markdown/Python/YAML/TOML, 24 for the stateless modes).
The two string-valued states are kept small: a C++ raw-string delimiter
is at most 16 characters, so `RawDelimiter` holds it inline (17 bytes),
and a shell heredoc tag, which has no limit, is a
`shared_ptr<string const>` shared by every line of the heredoc.
`ensureSynCache()` picks the mode's state type and tokenizer and hands
them to `fillSynCache<S>()`, which switches the variant (and drops the
old entries) when the mode's state type changed. `Buffer::render()` extends
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
| `src/utf8.hpp/.cpp`                    | UTF-8 decoding/encoding (`decodeUtf8`, `encodeUtf8`), character width (`charWidth`, `codepointWidth`), clusters (`charInfoAt` measures a character plus its combining marks as one unit; `clusterStart`, `prevClusterStart`, `nextClusterStart`, used by all cursor movement and character deletion in `BufferView`, `Buffer` and `Prompt`, so the cursor never rests between a character and its marks), `codepointClass` (block summary `kBlockClass` in front of one binary search), escaped characters — invisible format characters, line/paragraph separators and C1 controls — shown as `codepointEscape` escapes and orphaned marks drawn on `kOrphanMarkBase` ◌, `visualWidth`, `escapeNonPrintable`, `isWhitespace`, `readUtf8Tail`, `readUtf8Codepoint` |
| `src/unicode_tables.hpp`               | Generated (`tools/gen_unicode_tables.py`, Unicode 15.1 as in xfce4-terminal): `CodepointClass` (`Normal`, `Wide`, `Combining`, `Escaped`) and the sorted range table `kCodepointClasses`; looked up through `codepointClass()` in `utf8.hpp` |
| `src/prompt.hpp`                       | `Prompt` struct (label, callbacks, display cache, edit cursor, mark), `PromptDisplay`, `PromptHistory` (per-label input history)    |
| `src/paste_ring.hpp`                   | `PasteRing` — fixed-capacity ring buffer for clipboard history                                                               |
| `src/bookmarks.hpp`                    | `Bookmarks` — session-wide named positions (`Bookmark{bufferName, point}`) keyed by a single char in `[0-9a-z]`; owned by `Screen` |
| `src/clipboard.hpp/.cpp`               | X11 PRIMARY/CLIPBOARD integration via `xclip`/`xsel` subprocesses; lazy detection, async writes, sync reads                   |
| `src/error_log.hpp/.cpp`               | Appends diagnostic messages to `~/.cx-errors` for post-mortem inspection                                                     |
| `src/search.hpp`                       | Smart-find helpers (`matchAt`, `FindFlags`, `findFirstMatch`, `findLastMatch`) used by `Screen::findNext/findPrev`            |
| `src/replace.hpp`                      | Find-and-replace helpers (`findNextMatchInBuffer`, `replaceAllForward`) used by `Screen::openReplacePrompt`                  |
| `src/regex_search.hpp`                 | Regex search/replace helpers (`compileRegex`, `findFirstRegex`, `findNextRegexInBuffer`, `stepPastEmptyMatch`, `substituteCaptures`, `replaceAllRegexForward`) used by Esc-X / Esc-Y; mid-line searches keep the preceding text as context and scans step past empty matches; both replace-all helpers match each line once against its original text, rewrite it in one edit, and (bypassing BufferView) record one history version themselves |
| `src/mouse.hpp`                        | SGR mouse parser (`parseSgrMouse`, `MouseEvent`) — translates `ESC[<…M/m` payloads from the input loop into structured events |
| `src/cli.hpp`                          | `cli::parse` — turn argv into a `cli::Command` (`ShowHelp` / `ShowVersion` / `OpenFiles`); `cli::usage` help text            |
| `src/version.hpp/.cpp`                 | `versionText()` for `--version`; compiled into `cx` only, from the build-generated `build_info.hpp` (`cmake/build_info.cmake`, run every build, rewrites only on change) |
| `src/file_arg.hpp`                     | `parseFileArg` — split a `path:LINE` command-line argument into a path and an optional line number                          |
| `src/fs_path.hpp`                      | Non-throwing path helpers for user-supplied paths: `canonicalPath` (buffer keys in `BufferManager`/`DirBuffer`), `currentDir` |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup                                                                                                 |

---

### Performance characteristics

The text storage model (a chunked `LineStore` of `LinePtr`s, one
`shared_ptr<string const>` per line) has these characteristics:

- **Per-line overhead**: each line is a separate `std::string` plus a
  `shared_ptr` control block (~32 bytes overhead + allocator metadata).
  A file with millions of short lines wastes significant memory and
  fragments the heap. Snapshots and the live buffer share chunks and
  `LinePtr`s, so unchanged lines are not duplicated across versions.
- **Line splits/joins are O(T + N/T)**: inserting or deleting a line
  shifts pointers within one chunk (T ≈ 512) and updates the chunk
  index; the first edit of a chunk shared with a snapshot clones it
  (512 pointer copies).
- **Region deletion and large pastes** drop or build whole chunks;
  the cost is proportional to the region, not to the file.
- **Huge lines**: an edit to a line shared with a snapshot copies the
  whole line string, so history memory grows by the line's size per
  version (e.g. a 4 MB minified line).
- **Rendering is full-redraw**: `render()` rebuilds the entire screen
  string on every keystroke. This is adequate for typical terminal sizes
  but may cause lag on very large terminals over slow connections.
- **Syntax cache**: Per-line tokenisation results are cached and only
  recomputed from the edited line forward, avoiding full-file retokenisation
  on each keystroke.

For a production editor handling 100 MB+ files, a gap buffer, rope, or
piece table would be more appropriate. The current model is chosen for
simplicity and readability.
