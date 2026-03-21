# cx

A simple terminal text editor written in C++, using only the C++ standard library.

## Building

```sh
cmake -B build
cmake --build build
```

## Usage

```sh
./build/cx [file ...]
```

Open one or more files for editing. With no arguments, cx starts with an empty scratch buffer.

## Features

- **UTF-8 support** — full Unicode text editing and display
- **Multiple buffers** — open, switch between, and kill buffers; MRU-ordered buffer switching with tab completion and filesystem path completion
- **Buffer list** — interactive tree view of open buffers
- **Line wrapping** — toggle soft line wrap on/off
- **Whitespace display** — toggle visible whitespace markers
- **Mark and region** — set/clear mark, cut/copy/paste with a multi-entry paste ring and paste cycling
- **Word and paragraph navigation** — move by word, paragraph, or go to a specific line
- **Chunk deletion** — delete contiguous runs of whitespace or non-whitespace in either direction
- **Save / Save As** — write to the backing file or choose a new path
- **Revert** — reload a buffer's contents from disk
- **Built-in help** — adaptive side-by-side or stacked layout depending on terminal width

## Keyboard Shortcuts

### Commands

| Key | Action |
|-----|--------|
| Esc-? | Show help |
| Esc-c | Exit |
| Ctrl-o | Switch to buffer / open file |
| Esc-b | Buffer list |
| Ctrl-s | Save file |
| Ctrl-k | Kill buffer |
| Ctrl-r | Revert buffer from file |
| Esc-s | Toggle whitespace |
| Esc-l | Toggle line wrap |
| Ctrl-Space | Set / clear mark |
| Ctrl-w | Cut region |
| Esc-w | Copy region |
| Ctrl-y | Paste |
| Esc-y | Paste cycle |
| Esc-r | Paste ring |

### Movement

| Key | Action |
|-----|--------|
| Arrows | Move cursor |
| Ctrl-Left/Right | Move by word |
| Ctrl-Up/Down | Move by paragraph |
| Ctrl-g | Go to line |
| PgUp / PgDn | Page up / down (half screen) |
| Home / End | Start / end of line |
| Bksp | Delete before cursor |
| Ctrl-Bksp | Delete chunk before cursor |
| Delete | Delete at cursor |
| Ctrl-Del | Delete chunk at cursor |

## Goals

- Lightweight and dependency-free (C++ stdlib only)
- Simple, readable codebase
