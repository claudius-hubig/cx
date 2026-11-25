# cx

An AI-slopped, simple terminal text editor. Press Esc-? for help. cx has no configuration.

![cx editing its own source in three split panels](docs/screenshot.png)

The screenshot shows three panels that edit cx's own source, with line-number gutters and C-mode syntax highlighting.
The focused panel (bottom left) has three cursors. They show as reverse-video cells on the comment lines, and the
panel's modeline shows `3@I`. [The modeline](#the-modeline) explains the rest of that row.

## Building

```sh
cmake -B build
cmake --build build
```

Doxygen can generate the API documentation (optional):

```sh
cmake --build build --target docs   # output in build/docs/html
```

## Usage

```sh
./build/cx [OPTION]... [file[:LINE]|directory ...]
```

Open one or more files to edit them, or directories to browse them. With no arguments, cx starts with an empty
scratch buffer.

Add `:LINE` to a path argument (e.g. `cx src/foo.cpp:42`) to open the file at that 1-based line. cx uses the line
number of the last argument only.

cx opens a file or the output of a command. `cx file`, `cmd | cx` and `cx <(cmd)` all work.

Options:

- `-h` or `--help` prints a short usage summary.
- `--version` prints the git commit (with `-dirty` for uncommitted changes), the build date, the compiler, the build
  type and the compile flags.
- `--theme=dark` (the default) picks colours for a dark terminal background with light text. `--theme=light` picks
  colours for a light background with dark text.
- `--` ends option parsing, so `cx -- -notes.txt` opens a file whose name starts with `-`.

Any other unrecognised option is an error.

## Features

### Text and display

- cx edits and shows all Unicode text, including CJK wide characters and emoji, which take two columns. Combining
  marks (accents, emoji variation selectors) stay attached to the character before them. Character widths follow
  Unicode 15.1 and match xfce4-terminal.
- Invisible format characters show as dimmed escapes such as `​`, so they cannot hide in the text. These are
  the zero-width space, joiners, BOM, direction marks, bidi controls and the soft hyphen. cx escapes the C1 control
  characters (U+0080 to U+009F, which a terminal may act on) and the line and paragraph separators the same way.
- Soft line wrap can be on or off.
- Visible whitespace markers for spaces, tabs and newlines can be on or off.
- cx detects git merge conflict regions when it loads, reverts or saves a buffer. It draws the "ours", divider and
  "theirs" sections with different backgrounds, whatever the buffer's syntax mode.
- The bottom row of each panel is its modeline. It shows `line:col`, the active prompt or status message, the
  buffer's display name, its mode, a `+` when the buffer is modified, the cursor count, and an `I`/`O`
  insert/overwrite indicator. [The modeline](#the-modeline) describes each part.
- cx sets the terminal title to `cx@<cwd>` on startup, so you can find cx tabs easily.

### Buffers and files

- You can open, switch between and kill buffers. Buffer switching is in MRU order, with tab completion and
  filesystem path completion. The Ctrl-o switch prompt defaults to the previous buffer. Type `*name*` to create (or
  find) a scratch buffer of that name.
- The buffer list is an interactive tree of the open buffers. Directories with a single child are collapsed.
- The directory browser shows files with their permissions, owner, group and size. In it you can enter
  subdirectories, toggle hidden files, mark and delete entries, rename entries and create subdirectories. cx reads
  the listing again each time you open the directory, so `Enter` on `.` refreshes it.
- Save writes to the backing file, and Save As writes to a new path. Before it saves, cx asks for confirmation if the
  file no longer exists on disk, or if its mtime or size changed since load.
- Revert reloads a buffer from disk after you confirm. If another program changed a buffer's file since cx loaded
  or saved it, the status line says so when you switch to that buffer (once per change).
- cx reads a pipe or FIFO whole into a scratch buffer. Piped stdin goes into `*stdin*`. A FIFO argument such as
  `<(cmd)` goes into a buffer named after its path, such as `*/dev/fd/63*`. Ctrl-o opens a FIFO the same way. While
  it waits, the status line shows "Opening fifo <path>...". After 2 s with no data, cx asks whether to abort the
  open.
- When a file has CRLF or lone-CR line endings, cx asks whether to translate them to `\n`. A file with mixed endings
  gets a warning. cx asks once per load.

### Editing

- Set and clear the mark, then cut, copy and paste the region. A paste ring keeps several entries, and you can cycle
  through them when you paste.
- Text that the terminal pastes (Ctrl-Shift-V, Shift-Insert) goes in as it is, as a single edit and one undo step.
  cx applies no auto-indent or Tab formatting to it. A prompt takes the first line of a paste, and read-only buffers
  ignore it. This uses bracketed paste, which every current terminal supports. Without it, a paste still arrives as
  typed keys. If the terminal's end-of-paste marker gets lost, the paste ends after 2 s without input and the status
  line says so.
- Cut and Copy put the region on the X11 CLIPBOARD selection through `xclip` or `xsel`, if one is installed. Paste
  reads the CLIPBOARD when it differs from the top of the internal ring, so you can paste text that you copied with
  Ctrl-C in another application. After every keystroke, cx also copies the active region to the PRIMARY selection,
  so other applications can paste it with a middle click. A middle click inside the editor pastes the PRIMARY
  selection at the cursor. Without a graphical session, all X11 integration silently does nothing.
- `Esc-U`, `Esc-L` and `Esc-C` uppercase, lowercase and capitalize the active region. With no mark set, they change
  the word at the cursor, or the next word to its right. They change ASCII letters only. Non-ASCII letters stay as
  they are, and the edit never splits a multi-byte UTF-8 character.
- `Esc-q` refills the paragraph, or every paragraph in the region, to 80 columns. It keeps list bullets and
  indentation. In Markdown buffers it formats GFM pipe tables instead:
  - It aligns every column separator and honours `:--`, `:-:` and `--:` alignment.
  - It wraps the column under the cursor so that the table fits in 80 columns. A wrapped cell continues in rows whose
    first cell is empty, so cx never wraps the first column or the header row.
  - A wrapped column is at least 20 columns wide when its text needs that much.
  - It never splits code spans or links.
  - Tables inside block quotes (`> | a | b |`) work too.
  - With a region active, it aligns every table that the region touches as a whole.

  `Esc-q` never reflows or reformats fenced code blocks.
- Chunk deletion deletes a run of whitespace or non-whitespace in either direction.
- In C/C++, JSON and Python modes, Enter keeps the leading-spaces indent of the previous line.
- The `Insert` key switches the buffer between insert and overwrite mode. In overwrite mode, typing replaces the
  character under the cursor instead of pushing it right. At the end of a line typing appends, and Enter always
  inserts a newline. The modeline shows the mode as `I` or `O`, and the cursor shape follows it: a steady block for
  insert, a steady bar for overwrite.
- `Ctrl-u` opens a read-only history mode. `[` and `]` step to earlier and later versions. `Enter` (or `Ctrl-u`
  again) makes the selected version the new live state. `Esc-Esc` cancels and returns to the latest version. History
  survives save and revert.

### Search

- Find uses smart matching. It ignores case when the pattern is all lowercase. It skips digits and special
  characters in the buffer unless they appear in the pattern. The search runs on Enter, and the highlight stays
  visible for about 3 s. Press Ctrl-F again to go to the next match and enter a new search string. Inside the
  prompt, Ctrl-F and Ctrl-R repeat the previous search forward and backward without leaving the prompt. An empty
  prompt uses the last pattern again, which the prompt shows as a dim hint.
- `Esc-%` asks for a pattern and a replacement. It then walks forward through the matches and asks `y`/`n`/`!`/`q`
  at each one. `!` replaces all the remaining matches without asking. An empty replacement deletes each match.
  Matching is exact (case-sensitive, no fuzzy skipping), unlike the smart matching of Find.
- The Find, Replace and regex prompts accept newlines and tabs. Ctrl-q quotes any key, so Ctrl-q Enter inserts a
  newline. Tab inserts a tab, and a paste keeps all its lines. The prompt shows a newline as `\n` and a tab as `\t`.
- A pattern with a newline matches across lines. Replacing `abc` Ctrl-q Enter `def` with `abc def` joins those
  lines, and a newline in the replacement splits a line. Find and Replace match such a pattern exactly: its first
  line must end a line, its middle lines must be whole lines, and its last line must start the next line.
- `Esc-g` searches a whole tree. It lists the hits as `path:line: text` in a read-only `*grep*` buffer.
  - Inside a git work tree, it runs `git grep` from the repository root. That search honours `.gitignore` and leaves
    build output out of the results, because it matches tracked files only.
  - Outside a git work tree, it runs `grep -r` from the directory of the current buffer, or from the browsed
    directory when a directory buffer is open.

  `Enter` on a result opens that file at that line in the other panel. If only one panel is open, cx first splits it
  vertically. The result list stays visible while you walk through the matches. If a search runs longer than 10 s,
  cx shows the same "Kill? y/n" prompt as for other external commands.

### Navigation

- Move by word or paragraph, or go to a specific line. The go-to-line prompt also accepts `last` to jump to the end
  of the buffer.
- `Ctrl-N` and `Ctrl-B` jump between blocks. A block is a hunk header (`@@ ... @@`) in a diff buffer, or a conflict
  region (`<<<<<<<`/`=======`/`>>>>>>>`) in any buffer that contains one.

### Panels and mouse

- Split the terminal vertically or horizontally into up to 6 panels. Each panel has its own cursor and viewport.
  Focus a panel by its number, or go to the most recently used panel with C-x o.
- A left click puts the cursor at that position and focuses the panel if it is not focused. A left drag selects a
  region (from the `mark` to the cursor) and copies it to the X11 PRIMARY selection. A drag past the top or bottom
  of a panel scrolls it. A middle click pastes the X11 PRIMARY selection at the cursor. The wheel scrolls the panel
  under the pointer.
- While mouse tracking is on, cx blocks the terminal's own right-click menu and selection. Hold `Shift` while you
  click to bypass tracking, or turn tracking off with `C-x m`.

### External commands, signals and errors

- If an external command from Esc-f, Tab formatting or Ctrl-p runs longer than 10 s, cx shows a "Kill? y/n" prompt.
  So a hung formatter or pipe never freezes the editor. A kill stops everything that the command started, not only
  the command itself.
- cx handles signals and crashes as follows:
  - `SIGTERM` (a plain `kill`) acts as `Esc-c`. The editor asks about each modified buffer before it exits.
  - A second `SIGTERM` while the editor still asks makes an emergency save, and cx exits. The same happens when the
    terminal's input closes.
  - `SIGHUP` (the terminal went away), `SIGINT` and `SIGQUIT` end the editor at once, even when it does not respond.
    cx makes an emergency save on the way out.
  - A crash (`SIGSEGV`, `SIGABRT`, …) also makes an emergency save, on a best-effort basis.
- An emergency save never writes to a buffer's file. cx writes each modified buffer that has a file to a copy next
  to it, `<file>.unsaved-<UTC time>` (e.g. `notes.md.unsaved-20261115T143502Z`). The file stays as you last saved
  it, so you can compare the two and choose. cx restores the terminal first and then prints one line per buffer:
  - `cx: unsaved <file> written to <copy>`
  - `cx: could not save <file>: <reason>`
  - `cx: discarded <buffer>`, for a modified buffer that has no file
- cx appends uncaught exceptions, clipboard failures and similar problems to `~/.cx-errors`, for inspection after the
  fact. It never shows them in the UI.

## The modeline

The bottom row of every panel is its modeline. cx draws it in reverse video: bold on the focused panel, dim on the
others. The screenshot above shows three modelines. The left segment follows what you do. The right-aligned segment
describes the buffer:

```
3:17 | Created 3 cursors                                          notes.md [md] +   3@I
└──── left segment ────┘                                          └── right segment ──┘
```

In this example, the cursor is on line 3, character 17. The last event was `Esc-*`, which created three cursors.
The buffer is `notes.md` in Markdown mode. It has unsaved changes and 3 active cursors, in insert mode.

### Left segment

- `3:17` is the cursor position: the 1-based line, then the 1-based column. cx counts the column in *characters*,
  not bytes, so on a line of CJK or accented text it reports the column that you see.
- `| v 4/12  2026-07-19 15:04:22  [/] step  Ctrl-U commit` is the history-mode badge. It shows which version you
  are on out of how many, when cx captured that version, and the keys to step or commit.
- `| Find: rege` is the active prompt and the text that you typed, followed by any completion hint in dim text. The
  prompt cursor is in normal video against the bar. Inside a prompt:
  - `←`/`→` and `Home`/`End` move the cursor, and `Ctrl-←`/`Ctrl-→` move it by word.
  - `Bksp`/`Delete` delete a character, and `Ctrl-Bksp`/`Esc-Bksp`/`Ctrl-Del` delete a chunk.
  - `↑`/`↓` recall earlier input.
  - In prompts with completion, `Tab` cycles through the matches, and `→` at the end of the input accepts the
    current one.
  - `Ctrl-Space` sets the mark, and cursor movement then extends the selection (shown underlined). `Ctrl-W` and
    `Esc-w` cut and copy it. `Ctrl-Y` and `Esc-y` paste and cycle. All of these use the same paste ring and X11
    clipboard as the buffers. cx pastes only the first line of a multi-line entry.
  - A bare `Esc` cancels the prompt, so type the `Esc` chords as `Alt-w` and `Alt-y`, or press Esc quickly before
    the key.
- `| Created 3 cursors` is the most recent status message. It stays until the next keypress redraws the modeline.

The modeline shows either the prompt or the status message, never both, and only on the focused panel. The cursor
position and the history badge show on every panel.

### Right segment

- `notes.md` is the buffer's display name. It is the bare filename when that is unambiguous, and the full path
  otherwise. Built-in buffers have names in the form `*name*`: `*1*` (the scratch buffer that cx opens with),
  `*stdin*`, `*help*`, `*buffers*`, `*paste ring*` and `*grep*`.
- `[md]` is the buffer mode, one of `[txt]` `[c]` `[md]` `[jsn]` `[dif]` `[sh]` `[yml]` `[tml]` `[py]`. The mode
  controls syntax highlighting, auto-indent and formatting. `Esc-m` changes it.
- `+` means unsaved changes. A blank means the buffer matches the file on disk.
- `3@` is the cursor count while several cursors are active. It is blank with a single cursor, and shows `***@`
  above 999.
- `I` or `O` shows insert or overwrite mode, which the `Insert` key toggles. The terminal cursor shape follows it
  too: a block for insert, a bar for overwrite.

Read-only buffers show only the name. These are help, the directory browser, the buffer list, the paste ring, the
grep results, and any buffer that you browse in history mode. The mode, the cursor count and `I`/`O` do not apply to
a buffer that you cannot type into, so cx leaves them out.

The count and the `@` take a fixed-width field, so the `I`/`O` never changes column as cursors come and go:

```
notes.md [md] +   3@I     modified, 3 cursors, insert mode
notes.md [md] +     I     modified, single cursor, insert mode
notes.md [md]       O     saved, single cursor, overwrite mode
```

### When space runs short

The modeline never wraps or spills past its panel. As the panel gets narrower, the modeline gives up space in this
order:

1. cx truncates the buffer name from the front, with a leading `…`.
2. cx drops the name and keeps only the `I`/`O` block.
3. cx drops the `I`/`O` block too.

If a prompt is long enough to fill the whole line, cx truncates it from the back.

### Not on the modeline

Two cues near the modeline are easy to mistake for part of it:

- A dim digit in the **top** right corner of a panel is the panel number, for `C-x 1`…`C-x 6`. It shows only when
  more than one panel is open.
- The rightmost column of the panel body is the wrap and scroll gutter. `↵` marks a soft-wrapped line. `↠` marks a
  line whose text continues past the right edge in no-wrap mode.

## Keyboard shortcuts

### Files and buffers

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
| Esc-Esc | Dismiss overlay, or clear mark if there is none |

### Regions and editing

| Key        | Action                                        |
|------------|-----------------------------------------------|
| Ctrl-Space | Set / clear mark                              |
| Esc-Esc    | Clear mark                                    |
| Ctrl-w     | Cut region                                    |
| Esc-w      | Copy region                                   |
| Ctrl-y     | Paste                                         |
| Esc-y      | Paste cycle                                   |
| Esc-p      | Paste ring                                    |
| Esc-f      | Format buffer/region (C/C++)                  |
| Ctrl-p     | Pipe region through command                   |
| Esc-;      | Toggle line comment (C/C++, sh, py, yml, tml) |
| Esc-q      | Reflow paragraph / md table (80 cols)         |
| Esc-U      | Uppercase region/word                         |
| Esc-L      | Lowercase region/word                         |
| Esc-C      | Capitalize region/word                        |
| Tab        | Format line (C/C++)                           |

### Search and movement

| Key             | Action                                                     |
|-----------------|------------------------------------------------------------|
| Ctrl-f          | Find forward                                               |
| Ctrl-r          | Find backward                                              |
| Esc-%           | Find and replace                                           |
| Esc-X           | Regex find forward (see [Regular expressions](#regular-expressions)) |
| Esc-Y           | Regex find and replace (`$0..$9`/`$&` in the replacement)  |
| Esc-g           | Find in files (grep)                                       |
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

### Regular expressions

Esc-X and Esc-Y take patterns in a subset of ECMAScript (JavaScript) syntax. The subset needs no backtracking, and
cx matches it with its own engine. A search takes time linear in the length of the text, whatever the pattern, so no
pattern can freeze the editor. Esc or Ctrl-g cancels a long search over a big buffer.

These match a line break: `\n`, a newline that you type with Ctrl-q Enter or paste, `\s`, and a class that lists
it, such as `[\n ]`. Nothing else matches a line break: not `.`, not a negated class (`[^…]`, `\D`, `\W`, `\S`). So
a match spans lines only where the pattern says so. For example, `abc\ndef` finds `abc` at the end of a line and
`def` at the start of the next line. `\s*\n\s*` replaced by a space joins lines.

`^` and `$` match at the start and end of every line. Matching is case-sensitive. It works on characters (Unicode
code points) of the UTF-8 text, so `.` matches all of `é` or `中`, never one byte of it. Only `.` and negated
classes match a byte that is not valid UTF-8. A search that starts mid-line treats the text before the cursor as
context, so `^` matches only at a real line start. Patterns that can match empty text (`x*`, `^`, `$`, `\b`) work
for find, skip and replace-all.

| Pattern | Matches |
|---|---|
| `a`, `中`, `\.` `\*` `\\` … | the character itself (punctuation after `\` is literal) |
| `.` | any one character but a line break |
| `[abc]`, `[^abc]`, `[a-z]`, `[à-ÿ]` | one character of / not of the set (never a line break); ranges by code point |
| `\d` `\D` | an ASCII digit / any other character |
| `\w` `\W` | an ASCII letter, digit or `_` / any other character |
| `\s` `\S` | space, tab, line break, `\v`, `\f`, `\r` / any other character but a line break |
| `[[:alpha:]]` | POSIX class (ASCII), also `digit` `alnum` `upper` `lower` `space` `punct` `xdigit` `word` |
| `\n`, `\t`, `\r` | a line break, tab, carriage return |
| `\xHH`, `\uHHHH`, `\u{H…}` | the character with that hex code point |
| `^` `$` | start / end of a line |
| `\b` `\B` | at a word boundary / not at one |
| `x*` `x+` `x?` | `x` 0 or more / 1 or more / 0 or 1 times |
| `x{n}` `x{n,}` `x{n,m}` | `x` n times / n or more / n to m times (n, m ≤ 1000) |
| `x*?` `x+?` `x??` `x{n,m}?` | the same, lazy |
| `a\|b` | `a` or `b` |
| `(x)` / `(?:x)` | a group, captured as `$1`…`$9` / not captured |

Quantifiers are greedy. They take as many repetitions as still let the rest of the pattern match. The lazy forms
take as few as possible. On `<a><b>`, `<.*>` matches `<a><b>`, but `<.*?>` matches `<a>`. Of the matches that start
leftmost, the first one by these priorities wins, with alternatives tried left to right, as in ECMAScript.

In a replacement:

- `$&` or `$0` inserts the whole match.
- `$1`…`$9` insert a group. The group number is one digit, so `$10` is group 1 followed by a `0`.
- `$$` inserts a `$`.
- `` $` `` and `$'` insert the text of the line before and after the match.
- `\n`, `\t`, `\r` and `\\` insert a line break, tab, carriage return and backslash, as they match in a pattern.
  For example, replacing `(a)(b)` with `$1\n$2` splits the match over two lines.
- Any other backslash is literal.

cx refuses these with a message:

- back-references in the pattern (`(.)\1`) and lookahead or lookbehind (`(?=`, `(?!`, `(?<=`, `(?<!`), because they
  need backtracking
- named groups
- escapes of letters other than those above

`docs/regex-dialect.md` has the full specification.

### Multiple cursors

Several cursors can edit in parallel. Typing, Enter, Tab, Backspace, Delete, chunk delete and paste apply at every
cursor. One keystroke is one undo step, however many cursors are active. Arrow, word and Home/End movement moves
every cursor by the same rule.

Secondary cursors show as reverse-video cells. The modeline shows the live cursor count as `3@I`, in front of the
insert/overwrite indicator at its right end. The field is blank with a single cursor. It reserves three digit
columns, so the indicator never shifts as cursors come and go. Counts above 999 show as `***@`.

| Key            | Action                                                    |
|----------------|-----------------------------------------------------------|
| Esc-*          | Cursor at the end of every occurrence of the word at point |
| Ctrl-Shift-Down| Add cursor on the line below, at the same column           |
| Ctrl-Shift-Up  | Add cursor on the line above, at the same column           |
| Esc-Esc        | Back to a single cursor                                    |

To rename every occurrence of a word, press `Esc-*`, then `Ctrl-Bksp` to delete the word at every cursor, then type
the new name.

With a region marked, `Esc-*` matches the region's text literally instead of whole words. Any other command (find,
goto, mouse click, cut/copy, comment toggle, history mode, …) first collapses back to a single cursor.

Some terminals and tmux configurations swallow `Ctrl-Shift-Up/Down`. You can also run both actions by name through
`Esc-x`.

### Bookmarks

Bookmarks are named positions for the session, global across buffers. cx does not keep them after a restart.

| Key   | Action                                                       |
|-------|---------------------------------------------------------------|
| C-x b | Set bookmark (prompts for a slot: `0-9` or `a-z`)              |
| C-x j | Jump to bookmark (prompts for a slot; shows occupied slots)    |

### Bracket matching

When the cursor is on a bracket, `?` jumps to its match. This works in every mode:

- Cpp (`.c`, `.h`, `.cpp`, `.hpp`, …) matches `()[]{}` and template `<>`. It does not match plain comparison
  operators `<` and `>`. It skips brackets in strings, character literals and comments.
- Jsn (`.json`) matches `[]` and `{}`. It skips brackets inside strings.
- Md, Dif, Sh, Yml, Tml, Py and Txt (and any buffer with no specific mode) match `()[]{}` only. `<` and `>` are
  ordinary characters there.

If the cursor is not on a bracket, `?` inserts a `?` as usual.

### C mode

| Key | Action                             |
|-----|------------------------------------|
| Tab | Format current line (clang-format) |

### Directory browser

These shortcuts work in a directory listing.

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
| Esc-m | Set buffer mode (accepts `txt`/`text`, `c`/`cpp`/`c++`, `md`/`markdown`, `jsn`/`json`, `dif`/`diff`/`patch`, `sh`/`shell`/`bash`, `yml`/`yaml`, `tml`/`toml`, `py`/`python`) |
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

These keys work after you press `Ctrl-u` on a buffer with edit history.

| Key     | Action                                |
|---------|---------------------------------------|
| `[`     | Step to older version                 |
| `]`     | Step to newer version                 |
| Enter   | Commit selected version               |
| Ctrl-u  | Commit selected version               |
| Esc-Esc | Cancel and return to latest           |

### Prompts

These keys work in an active prompt (Find, Save As, Switch Buffer, Go to line, …):

| Key         | Action                                                         |
|-------------|----------------------------------------------------------------|
| Enter       | Accept input                                                   |
| Esc         | Cancel prompt                                                  |
| Up / Down   | Navigate per-prompt input history                              |
| Tab         | Cycle completion candidates (Save As, Switch Buffer)           |
| Right arrow | Accept current completion (and descend into directory if any)  |
| Bksp        | Remove one character (full UTF-8 codepoint)                    |
| `y/n/!/q`   | Replace prompt: yes / skip / all-rest / quit                   |
| Ctrl-q      | Insert the next key as it is (Enter: a newline)                |
| Tab         | Find, Replace and regex prompts: insert a tab                  |
| Ctrl-f      | Find prompt: advance forward without leaving the prompt        |
| Ctrl-r      | Find prompt: advance backward without leaving the prompt       |
