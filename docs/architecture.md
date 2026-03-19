# cx Architecture

## Overview

`cx` is a simple terminal text editor. Its architecture is split into two
layers: a **terminal layer** that handles raw I/O and rendering, and a
**buffer layer** that owns all text content and editing state. The two
layers are decoupled through the `BufferSink` interface so that tests can
exercise the buffer layer without a real terminal.

```
main()
  └── SetupTerminal   (RAII: raw mode, alternate screen)
  └── Screen          (terminal I/O, key dispatch, drawing)
        └── BufferManager  (buffer lifecycle, MRU, completions)
              └── Buffer*  (text, cursor, viewport, rendering)
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

### `Screen` (`src/terminal/screen.hpp/.cpp`)

Manages all terminal I/O. Owns the main event loop and the active
`Buffer*` pointer. Implements `BufferSink` so that `BufferManager` can
push buffer switches to it.

Key responsibilities:
- **Drawing**: `draw()` calls `Buffer::render()` to produce an ANSI escape
  string, then writes it to stdout along with a status line and cursor
  positioning.
- **Input loop**: `loop()` reads raw keystrokes, handles SIGWINCH, and
  dispatches to sub-handlers:
  - `handleConfirmSave` / `handleConfirmKill` — y/n confirmation prompts
  - `handleTextPrompt` — line-entry prompts (go-to-line, save-as,
    switch-buffer)
  - `handleEscapeInput` — CSI escape sequences (arrows, page keys) and
    Esc-prefix chords
  - `handleNormalInput` — printable characters and control keys in normal
    editing mode
- **Actions**: `handleSpecialInput()` maps control characters to `Action`
  enum values; the action dispatch in `handleNormalInput` implements the
  resulting editor commands.

`Screen` holds display state that belongs to the view rather than to any
buffer: wrap mode, whitespace visibility, the active prompt, and the
timed status message.

---

### `BufferSink` (`src/buffer/buffer_sink.hpp`)

Abstract interface with a single method `setBuffer(Buffer&)`. Decouples
`BufferManager` from `Screen` so that unit tests can supply a stub sink
without constructing a real terminal.

---

### `BufferManager` (`src/buffer/buffer_manager.hpp/.cpp`)

Owns all `TextBuffer` instances (keyed by canonical path or synthetic
name) plus the singleton `HelpBuffer` and lazily-created
`BufferListBuffer`. Maintains an MRU list for buffer switching.

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
  `moveWordLeft/Right`, `moveParagraphUp/Down`, `pageUp/Down`,
  `gotoLine`
- **Viewport management**: `ensureVisible`, `scrollUp/Down`,
  `scrollViewNearEnd`. The viewport is described by `_viewFromLine` (first
  visible physical line) and `_viewOffset` (a `std::variant` of
  `WrapOffset` for sub-line byte offsets in wrap mode, or `HScrollOffset`
  for horizontal scroll columns in no-wrap mode).
- **Rendering**: `render()` produces an ANSI escape string for the
  visible portion of the buffer. Returns the screen `{row, col}` of the
  cursor for hardware cursor positioning.

Editing methods are guarded by `readonly()` (a pure virtual that derived
classes override). The `_modified` flag is set on any edit and cleared by
a successful `save()`.

---

### `TextBuffer` (`src/buffer/text_buffer.hpp/.cpp`)

Concrete editable buffer backed by a file on disk. Reads the file on
construction; `save()` writes lines back as a truncating binary write and
returns `std::expected<void, std::string>` with an error message on
failure. Exposes `setPath()` for the save-as rename workflow.

---

### `HelpBuffer` (`src/buffer/help_buffer.hpp/.cpp`)

Readonly buffer that renders the key-binding reference. Overrides
`render()` and `screenRow()` to lay out command and movement sections
side-by-side when the terminal is wide enough, or stacked otherwise. The
content is derived from compile-time `std::string_view` arrays.

---

### `BufferListBuffer` (`src/buffer/buffer_list_buffer.hpp/.cpp`)

Readonly buffer that displays open buffers as a trie-based directory tree
with collapsed single-child nodes. Overrides `render()` to highlight the
selected line with reverse video. `selectedName()` returns the buffer name
under the cursor for use by `Screen` when the user presses Enter.

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

| File | Contents |
|------|----------|
| `src/types.hpp` | `RowIdx`, `ColIdx`, `BufferPoint`, `WrapOffset`, `HScrollOffset`, `ViewOffset` |
| `src/buffer/char_width.hpp` | `isWhitespace`, `charWidth`, `isUtf8Continuation`, `utf8SeqLen`, `decodeUtf8`, `codepointWidth`, `CharInfo`, `charInfoAt` |
| `src/terminal/setup_terminal.hpp/.cpp` | RAII terminal raw-mode setup |
| `src/error_log.hpp/.cpp` | Appends diagnostic messages to `cx_error.log` for post-mortem inspection |
