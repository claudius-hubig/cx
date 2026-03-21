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
  - `handleConfirmSave` / `handleConfirmKill` / `handleConfirmRevert` —
    y/n confirmation prompts
  - `handleTextPrompt` — line-entry prompts (go-to-line, save-as,
    switch-buffer, find)
  - `handleEscapeInput` — CSI escape sequences (arrows, page keys) and
    Esc-prefix chords
  - `handleNormalInput` — printable characters and control keys in normal
    editing mode
- **Action dispatch**: `dispatchAction(Action)` is the centralised switch
  that executes the effect of any `Action`. Both the Ctrl-key path and
  Esc-chord path resolve to an `Action` via `handleShortcut()`, then call
  `dispatchAction()`. Unrecognised shortcuts produce a status message.
- **Find**: `findNext()` implements forward search with smart matching
  (case-insensitive when the pattern is lowercase; digits and special
  characters in the buffer are skipped unless they appear in the pattern).
  On match, sets the cursor after the last matched character and applies
  a timed highlight.

`Screen` holds display state that belongs to the view rather than to any
buffer: wrap mode, whitespace visibility, the active prompt, find state,
the paste ring, and the timed status message.

---

### `BufferSink` (`src/buffer/buffer_sink.hpp`)

Abstract interface with a single method `setBuffer(Buffer&)`. Decouples
`BufferManager` from `Screen` so that unit tests can supply a stub sink
without constructing a real terminal.

---

### `BufferManager` (`src/buffer/buffer_manager.hpp/.cpp`)

Owns all `TextBuffer` instances (keyed by canonical path or synthetic
name) plus the singleton `HelpBuffer`, and lazily-created
`BufferListBuffer` and `PasteRingBuffer`. Maintains an MRU list for
buffer switching.

Key responsibilities:
- `open()` — canonicalizes a path and opens or returns an existing
  `TextBuffer`
- `create()` — creates a new empty `TextBuffer` with a synthetic name
- `kill()` — removes a buffer; always leaves at least one buffer alive
- `completions()` — returns match candidates for the switch-buffer prompt,
  covering both open buffers and filesystem entries
- `displayName()` — returns a short unambiguous name for status line and
  buffer-list display
- `touch()` / `secondMRU()` — MRU management for default buffer switching

---

### `Buffer` (`src/buffer/buffer.hpp/.cpp`)

Abstract base for all buffer types. Stores text as
`std::vector<std::vector<char>>` (one `vector<char>` per line, without
newline delimiters). Provides:

- **Editing**: `insert`, `deleteAtPoint`, `deleteBeforePoint`,
  `deleteChunkAtPoint`, `deleteChunkBeforePoint`
- **Cursor movement**: `moveLeft/Right/Up/Down`, `moveHome/End`,
  `moveWordLeft/Right`, `moveParagraphUp/Down`, `movePhysicalUp/Down`,
  `pageUp/Down`, `gotoLine`, `setPoint`
- **Mark and region**: `toggleMark`, `setMark`, `clearMark`,
  `extractRegion`, `deleteRegion`, `insertText`
- **Viewport management**: `ensureVisible`, `scrollUp/Down`,
  `scrollViewNearEnd`. The viewport is described by `ViewFrom` which
  holds a `RowIdx line` (first visible physical line) and a `ColIdx col`
  (byte offset for sub-line scroll in wrap mode, or horizontal scroll
  column in no-wrap mode).
- **Rendering**: `render()` produces an ANSI escape string for the
  visible portion of the buffer, including region highlighting (inverse
  video when mark is active) and find highlighting (light blue background
  via `setHighlight`/`clearHighlight`). Returns the screen `{row, col}`
  of the cursor for hardware cursor positioning.

Editing methods are guarded by `readonly()` (a pure virtual that derived
classes override). The `_modified` flag is set on any edit and cleared by
a successful `save()`.

---

### `TextBuffer` (`src/buffer/text_buffer.hpp/.cpp`)

Concrete editable buffer backed by a file on disk. Reads the file on
construction; `save()` writes lines back as a truncating binary write and
returns `std::expected<void, std::string>` with an error message on
failure. `revert()` reloads the buffer contents from its backing file,
resetting point, mark, viewport, and modified flag. Exposes `setPath()`
for the save-as rename workflow.

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
whenever a new buffer is switched to (from `open`, `create`, the
constructor's initial buffer, and `kill`'s replacement). `Screen`
implements `BufferSink::setBuffer()` by updating its `_buf` pointer and
calling `draw()`. This one-way dependency means `BufferManager` never
knows about `Screen` concretely.

---

### Supporting types and utilities

| File                                   | Contents                                                                                                                     |
|----------------------------------------|------------------------------------------------------------------------------------------------------------------------------|
| `src/types.hpp`                        | `RowIdx`, `ColIdx`, `BufferPoint`, `ViewFrom`                                                                                |
| `src/buffer/char_width.hpp`            | `isWhitespace`, `charWidth`, `isUtf8Continuation`, `utf8SeqLen`, `decodeUtf8`, `codepointWidth`, `CharInfo`, `charInfoAt`    |
| `src/utf8.hpp`                         | UTF-8 encoding helpers: `utf8Len`, `encodeUtf8`                                                                              |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup                                                                                                 |
| `src/error_log.hpp/.cpp`               | Appends diagnostic messages to `cx_error.log` for post-mortem inspection                                                     |
