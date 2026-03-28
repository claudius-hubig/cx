# cx — Project Guidelines

## Overview

`cx` is a simple terminal text editor written in C++. It uses **only the C++ standard library** — no external dependencies.

See `docs/architecture.md` for a detailed summary of the codebase.

## Build System

- CMake (minimum 3.10), C++23
- Build: `cmake -B build && cmake --build build`
- Binary: `./build/cx`
- Tests: `cmake --build build && ctest --test-dir build` (uses Boost.Test)

## Formatting

- Always run `clang-format --style=file -i` on every changed file before committing

## Constraints

- No external libraries or dependencies — stdlib only
- Target: C++23
- POSIX headers (`<termios.h>`, `<sys/ioctl.h>`, `<unistd.h>`) are a necessary exception for terminal I/O; there is no stdlib alternative

## Buffer Modes

- Mode names must be at most three letters (e.g. `txt`, `cpp`, `md`)
- Mode-specific code lives in `src/buffer/modes/<mode>/`
- The `BufferMode` enum (`src/buffer/buffer.hpp`) maps to mode-specific tokenizers for syntax highlighting
- Adding a new mode requires: a tokenizer in `src/buffer/modes/<mode>/`, an enum entry, detection logic in `TextBuffer::detectMode()`, and render integration in `Buffer::render()`

## Keyboard Shortcuts

- When adding or changing a keyboard shortcut, always update both the help text (`src/buffer/help_buffer.cpp`) and `README.md` to match
- All keybindings are defined in a single `constexpr` table in `src/shortcut.cpp` — this is the only place bindings should be added or changed
- Actions are dispatched through `Screen::dispatchAction()`

## Source Layout

```
src/
  main.cpp                         Entry point
  types.hpp                        RowIdx, ColIdx, BufferPoint, ViewFrom
  byte_constants.hpp               Named constants for terminal bytes (key::, ansi::)
  utf8.hpp/.cpp                    UTF-8 decode/encode, char width, visual width
  shortcut.hpp/.cpp                Action enum, Shortcut struct, binding table
  paste_ring.hpp                   Fixed-capacity ring buffer for cut/copy
  subprocess.hpp/.cpp              Run external commands (used for clang-format)
  error_log.hpp/.cpp               Diagnostic logging to cx_error.log
  terminal/
    setup_terminal.hpp/.cpp        RAII raw mode + alternate screen
    screen.hpp/.cpp                Terminal I/O, input loop, action dispatch, drawing
  buffer/
    buffer_sink.hpp                Abstract interface decoupling BufferManager from Screen
    buffer.hpp/.cpp                Abstract base: text storage, cursor, viewport, rendering
    text_buffer.hpp/.cpp           Editable file-backed buffer
    help_buffer.hpp/.cpp           Readonly keybinding reference
    buffer_list_buffer.hpp/.cpp    Readonly trie-based open-buffer listing
    paste_ring_buffer.hpp/.cpp     Readonly paste ring display
    dir_buffer.hpp/.cpp            Readonly directory browser with navigation and delete
    buffer_manager.hpp/.cpp        Buffer lifecycle, MRU, completions
    modes/
      c/syntax.hpp/.cpp            C/C++ tokenizer, bracket matching
      md/syntax.hpp/.cpp           Markdown tokenizer (headings, bold, links)
tests/
  test_helpers.hpp                 Shared test utilities
  test_buffer.cpp                  Buffer base class tests
  test_text_buffer.cpp             TextBuffer I/O tests
  test_help_buffer.cpp             HelpBuffer layout tests
  test_buffer_list_buffer.cpp      BufferListBuffer trie tests
  test_buffer_manager.cpp          BufferManager lifecycle tests
  test_dir_buffer.cpp              DirBuffer navigation/delete tests
  test_render.cpp                  Render output tests
  test_movement.cpp                Cursor movement tests
  test_scroll.cpp                  Viewport scrolling tests
  test_chunk_delete.cpp            Word-level deletion tests
  test_char_width.cpp              UTF-8 / character width tests
  test_shortcut.cpp                Shortcut table tests
  test_syntax.cpp                  Syntax highlighting tests
  test_e2e.cpp                     End-to-end tests (via PTY)
```
