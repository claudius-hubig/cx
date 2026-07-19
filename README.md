# cx

AI-slopped, simple, terminal text editor. Esc-? for help text. No configuration.

## Building

```sh
cmake -B build
cmake --build build
```

API documentation can be generated with Doxygen (optional):

```sh
cmake --build build --target docs   # output in build/docs/html
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
- **Multiple buffers** — open, switch between, and kill buffers; MRU-ordered buffer switching with tab completion and filesystem path completion. The Ctrl-o switch prompt defaults to the previous buffer; typing `*name*` creates (or finds) a scratch buffer of that name.
- **Buffer list** — interactive tree view of open buffers (collapsed single-child directories)
- **Find** — smart search: case-insensitive when the pattern is all lowercase; digits and special characters in the buffer are transparently skipped unless they appear in the search pattern. The search runs on Enter (highlight visible for ~3 s); Ctrl-F again advances to the next match while re-prompting for a new search string. Inside the prompt Ctrl-F / Ctrl-R also advance the previous search forward/backward without leaving the prompt. An empty prompt re-uses the last pattern (shown as a dim hint).
- **Find and replace** — `Esc-%` prompts for a pattern and replacement, then walks forward through matches asking `y`/`n`/`!`/`q` per hit. `!` finishes the rest without prompting. Matching is exact (case-sensitive, no fuzzy skipping) — unlike Find, which uses smart matching for navigation.
- **Mark and region** — set/clear mark, cut/copy/paste with a multi-entry paste ring and paste cycling
- **Case conversion** — `Esc-U` / `Esc-L` / `Esc-C` uppercase / lowercase / capitalize the active region, or the word at (or next to the right of) the cursor when no mark is set. ASCII-only: non-ASCII letters are left unchanged, and the edit never splits a multi-byte UTF-8 character.
- **System clipboard integration (X11)** — Cut and Copy publish the region to the X11 CLIPBOARD selection (via `xclip` or `xsel` if installed); Paste reads the CLIPBOARD when it differs from the top of the internal ring, so external Ctrl-C from other apps becomes pasteable. The active mark region is also mirrored to the PRIMARY selection after every keystroke (so other apps can middle-click-paste it), and middle-click inside the editor pastes the PRIMARY selection back at the cursor. All X11 integration is silently a no-op when no graphical session is available.
- **Word and paragraph navigation** — move by word, paragraph, or go to a specific line (the prompt also accepts `last` to jump to end of buffer)
- **Block navigation** — `Ctrl-N` / `Ctrl-B` jump between blocks: hunk headers (`@@ ... @@`) in diff buffers, and conflict regions (`<<<<<<<`/`=======`/`>>>>>>>`) in any buffer that contains them.
- **Conflict-marker highlighting** — git merge conflict regions are detected on load/revert/save and rendered with distinct backgrounds for the "ours", divider, and "theirs" sections, regardless of the buffer's syntax mode.
- **Chunk deletion** — delete contiguous runs of whitespace or non-whitespace in either direction
- **Line wrapping** — toggle soft line wrap on/off
- **Whitespace display** — toggle visible whitespace markers (spaces, tabs, newlines)
- **Save / Save As** — write to the backing file or choose a new path. Saving prompts for confirmation if the file no longer exists on disk, or if its mtime/size has changed since load (external-modification guard).
- **Revert** — reload a buffer's contents from disk with confirmation
- **Auto-indent** — pressing Enter in C/C++, JSON, or Python modes preserves the leading-spaces indent of the previous line.
- **Overwrite mode** — the `Insert` key toggles between insert and overwrite editing (per buffer). In overwrite mode typing replaces the character under the cursor instead of pushing it right; at end of line it appends, and Enter always inserts a newline. The active mode shows as `I`/`O` on the modeline and as the cursor shape (steady block for insert, steady bar for overwrite).
- **Line-ending detection** — files loaded with CRLF or lone-CR endings prompt to translate to `\n`; mixed-ending files surface a warning. Acknowledged once per load.
- **Undo / version history** — `Ctrl-u` enters a read-only history mode: `[` and `]` step through earlier/later versions, `Enter` commits the selected version as the new live state, `Esc-Esc` cancels and returns to the latest. (Pressing `Ctrl-u` again is equivalent to `Enter`.) History survives save and revert.
- **Directory browser** — open directories to see files with permissions, owner, group, and size; navigate into subdirectories; toggle hidden files; mark and delete entries; rename entries; create subdirectories
- **Split panels** — split the terminal vertically or horizontally into up to 6 independent panels, each with its own cursor and viewport; focus panels by number or switch to the most recently used panel with C-x o
- **Mouse support** — left-click positions the cursor (and focuses a non-focused panel); left-drag selects a region (the `mark`→cursor span, mirrored to the X11 PRIMARY selection), autoscrolling when dragged past the top or bottom of a panel. Middle-click pastes the X11 PRIMARY selection at the cursor. The wheel scrolls the panel under the pointer. While mouse tracking is on, the terminal's own right-click context menu and selection are suppressed — hold `Shift` while clicking to bypass tracking, or toggle tracking off with `C-x m`.
- **Long-running command kill prompt** — external commands launched via Esc-f, Tab-format, or Ctrl-p that take longer than 10 s show a "Kill? y/n" prompt so a hung formatter or pipe never freezes the editor.
- **Modeline** — the bottom row of each panel shows `line:col`, the active prompt or status message, the buffer's display name, its mode (`[txt]` / `[c]` / `[md]` / `[jsn]` / `[dif]` / `[sh]` / `[yml]` / `[py]`), an `I`/`O` insert/overwrite indicator, and a `+` indicator when modified. A history-mode badge replaces the status segment while browsing versions.
- **Terminal title** — set to `cx@<cwd>` on startup so cx tabs are easy to identify.
- **Error log** — uncaught exceptions, clipboard failures and the like are appended to `~/.cx-errors` for post-mortem inspection (never surfaced as UI noise).

## Keyboard Shortcuts

### Files & Buffers

| Key     | Action                                          |
|---------|-------------------------------------------------|
| Esc-?   | Show help (press again to dismiss)              |
| Esc-c   | Exit (prompts to save modified buffers)         |
| Ctrl-o  | Switch to buffer / open file (defaults to MRU)  |
| Esc-b   | Buffer list                                     |
| Esc-d   | Directory browser                               |
| Ctrl-s  | Save file                                       |
| C-x k   | Kill buffer                                     |
| Esc-r   | Revert buffer from file                         |
| Ctrl-u  | Undo / version history                          |
| Esc-x   | Execute command by name (substring completion)  |
| q       | Dismiss read-only overlay (help / list / ring)  |
| Esc-Esc | Dismiss overlay; otherwise clear mark           |

### Regions & Edit

| Key        | Action                       |
|------------|------------------------------|
| Ctrl-Space | Set / clear mark             |
| Esc-Esc    | Clear mark                   |
| Ctrl-w     | Cut region                   |
| Esc-w      | Copy region                  |
| Ctrl-y     | Paste                        |
| Esc-y      | Paste cycle                  |
| Esc-p      | Paste ring                   |
| Esc-f      | Format buffer/region (C/C++) |
| Ctrl-p     | Pipe region through command  |
| Esc-;      | Toggle line comment (C/C++)  |
| Esc-q      | Reflow paragraph (80 cols)   |
| Esc-U      | Uppercase region/word        |
| Esc-L      | Lowercase region/word        |
| Esc-C      | Capitalize region/word       |
| Tab        | Format line (C/C++)          |

### Search & Movement

| Key             | Action                                                     |
|-----------------|------------------------------------------------------------|
| Ctrl-f          | Find forward                                               |
| Ctrl-r          | Find backward                                              |
| Esc-%           | Find and replace                                           |
| Esc-X           | Regex find forward (ECMAScript / `std::regex` syntax)      |
| Esc-Y           | Regex find and replace (`$0..$9`/`$&` back-references)     |
| Arrows          | Move cursor                                                |
| Ctrl-Left/Right | Move by word                                               |
| Ctrl-Up/Down    | Move by paragraph                                          |
| Ctrl-g          | Go to line (accepts a number, or `last` for end of buffer) |
| Ctrl-n          | Next block (`@@` hunk in diff buffers / conflict marker)   |
| Ctrl-b          | Prev block (same)                                          |
| Ctrl-l          | Recenter cursor (cycles middle / top / bottom of screen)   |
| PgUp / PgDn     | Page up / down (half screen)                               |
| Home / End      | Start / end of line                                        |
| Ctrl-Home/End   | Start / end of file                                        |

### Bookmarks

Session-wide named positions, global across buffers (not persisted across restarts).

| Key   | Action                                                       |
|-------|---------------------------------------------------------------|
| C-x b | Set bookmark (prompts for a slot: `0-9` or `a-z`)              |
| C-x j | Jump to bookmark (prompts for a slot; shows occupied slots)    |

### Bracket matching

Pressing `?` while the cursor is on a bracket jumps to its match. Available
in every mode:

- **Cpp** (`.c`, `.h`, `.cpp`, `.hpp`, …): matches `()[]{}` and template
  `<>`. Plain comparison operators `<`/`>` are not matched. Brackets in
  strings, character literals and comments are skipped.
- **Jsn** (`.json`): matches `[]` and `{}`. Brackets inside strings are
  skipped.
- **Md / Dif / Sh / Yml / Py / Txt** (and any buffer with no specific
  mode): matches `()[]{}` only. `<` and `>` are treated as ordinary
  characters.

If the cursor is not on a bracket, `?` is inserted normally.

### C mode

| Key | Action                             |
|-----|------------------------------------|
| Tab | Format current line (clang-format) |

### Directory Browser

These shortcuts are active when viewing a directory listing.

| Key   | Action                          |
|-------|---------------------------------|
| Enter | Open file / enter directory     |
| h     | Toggle hidden files (dotfiles)  |
| d     | Mark / unmark for deletion      |
| x     | Delete marked items             |
| r     | Rename selected entry           |
| m     | Make subdirectory               |

### Display

| Key   | Action                                                                              |
|-------|-------------------------------------------------------------------------------------|
| Esc-s | Toggle whitespace                                                                   |
| Esc-l | Toggle line wrap                                                                    |
| Esc-_ | Toggle highlight on the cursor's line                                               |
| Esc-# | Toggle line-number gutter                                                           |
| Esc-m | Set buffer mode (accepts `txt`/`text`, `c`/`cpp`/`c++`, `md`/`markdown`, `jsn`/`json`, `dif`/`diff`/`patch`, `sh`/`shell`/`bash`, `yml`/`yaml`, `py`/`python`) |
| Insert | Toggle insert/overwrite mode                                                       |

### Panels

| Key      | Action                 |
|----------|------------------------|
| C-x v    | Split panel vertical   |
| C-x h    | Split panel horizontal |
| C-x .    | Close panel            |
| C-x o    | Other panel (MRU)      |
| C-x 1..6 | Focus panel 1..6       |
| C-x m    | Toggle mouse tracking  |

### Deletion

| Key       | Action                     |
|-----------|----------------------------|
| Bksp      | Delete before cursor       |
| Ctrl-Bksp | Delete chunk before cursor |
| Esc-Bksp  | Delete chunk before cursor |
| Delete    | Delete at cursor           |
| Ctrl-Del  | Delete chunk at cursor     |
| Ctrl-k    | Delete current line        |

### History mode

Active after pressing `Ctrl-u` on a buffer with edit history.

| Key     | Action                                |
|---------|---------------------------------------|
| `[`     | Step to older version                 |
| `]`     | Step to newer version                 |
| Enter   | Commit selected version               |
| Ctrl-u  | Commit selected version               |
| Esc-Esc | Cancel and return to latest           |

### Prompts

When a prompt is active (Find, Save As, Switch Buffer, Go to line, …):

| Key         | Action                                                         |
|-------------|----------------------------------------------------------------|
| Enter       | Accept input                                                   |
| Esc         | Cancel prompt                                                  |
| Up / Down   | Navigate per-prompt input history                              |
| Tab         | Cycle completion candidates (Save As, Switch Buffer)           |
| Right arrow | Accept current completion (and descend into directory if any)  |
| Bksp        | Remove one character (full UTF-8 codepoint)                    |
| `y/n/!/q`   | Replace prompt: yes / skip / all-rest / quit                   |
| Ctrl-f      | Find prompt: advance forward without leaving the prompt        |
| Ctrl-r      | Find prompt: advance backward without leaving the prompt       |
