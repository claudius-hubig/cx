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
`Screen::loop()` to run the editor.

---

### `SetupTerminal` (`src/terminal/setup_terminal.hpp/.cpp`)

RAII wrapper around terminal configuration. On construction, saves the
current `termios` state and puts stdin into raw mode; switches the
terminal to the alternate screen buffer. On destruction, restores the
saved state and returns to the normal screen. Guarantees cleanup even on
exception.

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

Actions: `Help`, `Exit`, `GoToLine`, `Save`, `KillBuffer`,
`SwitchBuffer`, `ToggleMark`, `Cut`, `Copy`, `Paste`, `PasteCycle`,
`DeleteChunkBefore`, `Revert`, `Find`, `FindBackward`, `BufferList`,
`PasteRing`, `ToggleWhitespace`, `ToggleWrap`, `Format`, `SetMode`,
`DirBrowser`.

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
  - `handleConfirm` — unified Y/N/Esc confirmation handler for save,
    kill, revert, and delete confirmations
  - `handleTextPrompt` — line-entry prompts (go-to-line, save-as,
    switch-buffer, find, set-mode)
  - `handleEscapeInput` — CSI escape sequences (arrows, page keys) and
    Esc-prefix chords
  - `handleNormalInput` — printable characters and control keys in normal
    editing mode
- **Action dispatch**: `dispatchAction(Action)` is the centralised switch
  that executes the effect of any `Action`. Both the Ctrl-key path and
  Esc-chord path resolve to an `Action` via `handleShortcut()`, then call
  `dispatchAction()`. Unrecognised shortcuts produce a status message.
- **Find**: `findNext()` and `findPrev()` implement forward and backward
  search with smart matching (case-insensitive when the pattern is
  lowercase; digits and special characters in the buffer are skipped
  unless they appear in the pattern). On match, sets the cursor after the
  last matched character and applies a timed highlight.

`Screen` holds display state that belongs to the view rather than to any
buffer: wrap mode, whitespace visibility, the active prompt, find state,
the paste ring, and the timed status message.

Prompts: `None`, `GoToLine`, `SaveAs`, `SwitchBuffer`, `ConfirmSave`,
`ConfirmKill`, `ConfirmRevert`, `ConfirmDelete`, `Find`, `SetMode`.

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

Abstract base for all buffer types. Stores text as
`std::vector<std::string>` (one `std::string` per line, without newline
delimiters). Provides:

- **Editing**: `insert`, `deleteAtPoint`, `deleteBeforePoint`,
  `deleteChunkAtPoint`, `deleteChunkBeforePoint`
- **Cursor movement**: `moveLeft/Right/Up/Down`, `moveHome/End`,
  `moveWordLeft/Right`, `moveParagraphUp/Down`, `movePhysicalUp/Down`,
  `pageUp/Down`, `gotoLine`, `gotoMatchingBracket`, `setPoint`
- **Mark and region**: `toggleMark`, `setMark`, `clearMark`,
  `extractRegion`, `deleteRegion`, `insertText`
- **Viewport management**: `ensureVisible`, `scrollUp/Down`,
  `scrollViewNearEnd`. The viewport is described by `ViewFrom` which
  holds a `RowIdx line` and a mode-dependent sub-position: `ByteOffset`
  in wrap mode (sub-line scroll), or `VisualCol` in no-wrap mode
  (horizontal scroll).
- **Rendering**: `render()` produces an ANSI escape string for the
  visible portion of the buffer, including syntax highlighting (via
  per-mode tokenizers), region highlighting (inverse video when mark is
  active), and find highlighting (light blue background via
  `setHighlight`/`clearHighlight`). Returns the screen `{row, col}`
  of the cursor for hardware cursor positioning.
- **Formatting**: `format()` runs an external formatter (clang-format in
  Cpp mode) via `runSubprocess()`. `receiveTab()` triggers per-line
  formatting in Cpp mode, or inserts a literal tab otherwise.
- **Mode**: `BufferMode` enum (`Txt`, `Cpp`, `Md`) determines syntax
  highlighting, auto-indent, and formatting behaviour. Changed via
  `setMode()`.

Editing methods are guarded by `readonly()` (a pure virtual that derived
classes override). The `_modified` flag is set on any edit and cleared by
a successful `save()`.

Caching:
- `_pointColCache` — memoises visual and codepoint column for the current
  point position.
- `_rowBoundsCache` — caches visual-row boundary offsets for a single
  line (used by wrapped-line navigation).
- `_synCache` — per-line syntax tokenisation cache, invalidated on edits.

---

### `TextBuffer` (`src/buffer/text_buffer.hpp/.cpp`)

Concrete editable buffer backed by a file on disk. Reads the file on
construction; `save()` writes lines back as a truncating binary write and
returns `std::expected<void, std::string>` with an error message on
failure. `revert()` reloads the buffer contents from its backing file,
resetting point, mark, viewport, and modified flag. Exposes `setPath()`
for the save-as rename workflow. `detectMode()` sets the `BufferMode`
based on file extension (`.c`, `.h`, `.cpp`, `.hpp` → `Cpp`; `.md` → `Md`;
otherwise `Txt`).

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

Each entry is represented as a `DirBuffer::Entry` struct with metadata
gathered from the filesystem. `refresh()` rebuilds the listing from disk.

---

### `PasteRing` (`src/paste_ring.hpp`)

Fixed-capacity ring buffer for cut/copied text. Stores entries as
`std::string` with embedded `'\n'` for line breaks. Index 0 is the most
recent entry. Used by `Screen` to implement cut, copy, paste, and paste
cycling.

---

### `PasteRingBuffer` (`src/buffer/paste_ring_buffer.hpp/.cpp`)

Readonly buffer that displays the paste ring contents. Similar pattern to
`BufferListBuffer` — `refresh()` rebuilds lines from the current ring
state.

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
  strings with `R"delim(...)delim"`), operators, and brackets (parens,
  brackets, braces, angles). Maintains multi-line state (`SyntaxState`)
  for block comments and raw string literals. Also provides
  `findMatchingBracket()` for jump-to-matching-bracket support.

- **Markdown mode** (`modes/md/syntax.hpp/.cpp`): Tokenizes headings
  (`# ...`), bold (`**...**`), and links (`[text](url)`). No multi-line
  state needed.

The `SyntaxToken` enum (`Normal`, `Keyword`, `Comment`, `String`,
`Operator`, `Paren`, `Bracket`, `Brace`, `Angle`, `Heading`, `Bold`,
`Link`) is shared across modes. Tokens are mapped to ANSI colours in
`Buffer::render()`.

Per-line tokenisation results are cached in `Buffer::_synCache` and
invalidated from the edited line forward. Lines above the viewport
use the cheaper `advanceBlockCommentState()` to maintain only the
multi-line state without producing tokens.

---

### Supporting types and utilities

| File                                   | Contents                                                                                                                     |
|----------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `src/types.hpp`                        | `RowIdx`, `ColIdx`, `contentWidth()`, `BufferPoint`, `ByteOffset`, `VisualCol`, `ViewFrom`                                   |
| `src/byte_constants.hpp`               | Named constants for terminal input bytes (`key::` namespace) and ANSI output sequences (`ansi::` namespace incl. `cursorTo()`) |
| `src/utf8.hpp/.cpp`                    | UTF-8 decoding/encoding (`decodeUtf8`, `encodeUtf8`), character width (`charWidth`, `codepointWidth`, `charInfoAt`), `visualWidth`, `isWhitespace`, `readUtf8Tail`, `readUtf8Codepoint` |
| `src/subprocess.hpp/.cpp`              | `runSubprocess()` — fork/exec with stdin pipe, captures stdout/stderr. Used by `Buffer::format()` to run clang-format        |
| `src/paste_ring.hpp`                   | `PasteRing` — fixed-capacity ring buffer for clipboard history                                                               |
| `src/error_log.hpp/.cpp`               | Appends diagnostic messages to `cx_error.log` for post-mortem inspection                                                     |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup                                                                                                 |

---

### Performance characteristics

The text storage model (`vector<string>`, one heap allocation per line)
is simple but has known scalability limits:

- **Per-line overhead**: each line is a separate `std::string` with its
  own heap allocation (~32 bytes overhead + allocator metadata). A file
  with millions of short lines wastes significant memory and fragments the
  heap.
- **Line splits/joins are O(N)**: inserting or deleting a newline calls
  `_lines.insert()`/`_lines.erase()`, which shifts all subsequent line
  strings. On a multi-million-line file this is noticeable.
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
