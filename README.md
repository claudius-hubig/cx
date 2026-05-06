# cx

A simple terminal text editor. Esc-? for help text. No configuration.

## Building

```sh
cmake -B build
cmake --build build
```

## Usage

```sh
./build/cx [file|directory ...]
```

Open one or more files for editing, or directories for browsing. With no arguments, cx starts with an empty scratch buffer.

A trailing `:LINE` on a path argument (e.g. `cx src/foo.cpp:42`) opens the file at the given 1-based line. Only the final argument's line number is honoured.

If stdin is a pipe (e.g. `cmd | cx`), cx reads it into a `*stdin*` scratch buffer.

## Features

- **UTF-8 support** — full Unicode text editing and display, including CJK wide characters
- **Multiple buffers** — open, switch between, and kill buffers; MRU-ordered buffer switching with tab completion and filesystem path completion
- **Buffer list** — interactive tree view of open buffers (collapsed single-child directories)
- **Find** — smart search: case-insensitive when the pattern is all lowercase; digits and special characters in the buffer are transparently skipped unless they appear in the search pattern. The search runs on Enter; Ctrl-F again advances to the next match while re-prompting for a new search string. Matched text is temporarily highlighted.
- **Find and replace** — `Esc-%` prompts for a pattern and replacement, then walks forward through matches asking `y`/`n`/`!`/`q` per hit. `!` finishes the rest without prompting.
- **Mark and region** — set/clear mark, cut/copy/paste with a multi-entry paste ring and paste cycling
- **Word and paragraph navigation** — move by word, paragraph, or go to a specific line
- **Chunk deletion** — delete contiguous runs of whitespace or non-whitespace in either direction
- **Line wrapping** — toggle soft line wrap on/off
- **Whitespace display** — toggle visible whitespace markers (spaces, tabs, newlines)
- **Save / Save As** — write to the backing file or choose a new path
- **Revert** — reload a buffer's contents from disk with confirmation
- **Undo / version history** — `Ctrl-u` enters a read-only history mode: `[` and `]` step through earlier/later versions, and `Ctrl-u` again commits the selected version as the new live state. History survives save and revert.
- **Directory browser** — open directories to see files with permissions, owner, group, and size; navigate into subdirectories; toggle hidden files; mark and delete entries
- **Split panels** — split the terminal vertically or horizontally into up to 6 independent panels, each with its own cursor and viewport; focus panels by number or switch to the most recently used panel with C-x o

## Keyboard Shortcuts

### Files & Buffers

| Key    | Action                       |
|--------|------------------------------|
| Esc-?  | Show help                    |
| Esc-c  | Exit                         |
| Ctrl-o | Switch to buffer / open file |
| Esc-b  | Buffer list                  |
| Esc-d  | Directory browser            |
| Ctrl-s | Save file                    |
| C-x k  | Kill buffer                  |
| Esc-r  | Revert buffer from file      |
| Ctrl-u | Undo / version history       |

### Regions & Edit

| Key        | Action                       |
|------------|------------------------------|
| Ctrl-Space | Set / clear mark             |
| Ctrl-w     | Cut region                   |
| Esc-w      | Copy region                  |
| Ctrl-y     | Paste                        |
| Esc-y      | Paste cycle                  |
| Esc-p      | Paste ring                   |
| Esc-f      | Format buffer/region (C/C++) |
| Ctrl-p     | Pipe region through command  |
| Esc-;      | Toggle line comment (C/C++)  |
| Tab        | Format line (C/C++)          |

### Search & Movement

| Key             | Action                       |
|-----------------|------------------------------|
| Ctrl-f          | Find forward                 |
| Ctrl-r          | Find backward                |
| Esc-%           | Find and replace             |
| Arrows          | Move cursor                  |
| Ctrl-Left/Right | Move by word                 |
| Ctrl-Up/Down    | Move by paragraph            |
| Ctrl-g          | Go to line                   |
| Ctrl-n          | Next block (hunk/conflict)   |
| Ctrl-b          | Prev block (hunk/conflict)   |
| PgUp / PgDn     | Page up / down (half screen) |
| Home / End      | Start / end of line          |
| Ctrl-Home/End   | Start / end of file          |

### C mode

These shortcuts are active only when the buffer mode is set to `c` (via Esc-m).

| Key | Action                             |
|-----|------------------------------------|
| ?   | Jump to matching bracket           |
| Tab | Format current line (clang-format) |

Note: `?` jumps to the matching bracket when the cursor is on `(`, `)`, `[`, `]`,
`{`, `}`, `<`, or `>`. If the cursor is not on a bracket, `?` is inserted normally.
Template angle brackets `<>` are matched; comparison operators `<` and `>` are not.

### JSON mode

These shortcuts are active only when the buffer mode is set to `jsn` (via Esc-m).
Auto-enabled for `.json` files.

| Key | Action                        |
|-----|-------------------------------|
| ?   | Jump to matching bracket      |

Note: `?` jumps to the matching bracket when the cursor is on `[`, `]`,
`{`, or `}`. Brackets inside strings are ignored. If the cursor is not on a
bracket, `?` is inserted normally.

### Directory Browser

These shortcuts are active when viewing a directory listing.

| Key   | Action                          |
|-------|---------------------------------|
| Enter | Open file / enter directory     |
| h     | Toggle hidden files (dotfiles)  |
| d     | Mark / unmark for deletion      |
| x     | Delete marked items             |

### Display

| Key   | Action              |
|-------|---------------------|
| Esc-s | Toggle whitespace   |
| Esc-l | Toggle line wrap    |
| Esc-m | Set buffer mode     |

### Panels

| Key      | Action                 |
|----------|------------------------|
| C-x v    | Split panel vertical   |
| C-x h    | Split panel horizontal |
| C-x .    | Close panel            |
| C-x o    | Other panel (MRU)      |
| C-x 1..6 | Focus panel 1..6       |

### Deletion

| Key       | Action                     |
|-----------|----------------------------|
| Bksp      | Delete before cursor       |
| Ctrl-Bksp | Delete chunk before cursor |
| Delete    | Delete at cursor           |
| Ctrl-Del  | Delete chunk at cursor     |
| Ctrl-k    | Delete current line        |
