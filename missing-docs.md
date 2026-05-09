# Documentation drift in `cx`

A comparison of the user-facing implementation against:

- `README.md` — user-facing introduction, feature list and shortcut tables.
- `docs/architecture.md` — class-level summary of the codebase.
- `docs/highlighting.md` — syntax highlighting reference.
- `docs/potential_improvements.md` — one-item TODO list.

The three documents are largely consistent with each other where they
overlap, but a number of user-visible features and implementation
details have been added in code without a corresponding documentation
update. Items below are grouped by where they should be added (or
corrected). Each item is a defect — the docs guideline in `CLAUDE.md`
explicitly states that drift is a defect.

---

## 1. README.md

### 1.1 Missing from the **Features** list

| Feature                              | Where it is implemented                                                                    | Why it matters to a user                                                                                                                                |
|--------------------------------------|--------------------------------------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Line-ending detection**            | `TextBuffer::lineEndingStyle()` + `Screen::checkLineEndings()` (`screen.cpp:246`)          | On opening a CRLF or lone-CR file, cx prompts to translate to `\n`. Mixed endings show a status warning. Completely undocumented in the README.         |
| **External-modification guard**      | `TextBuffer::changedOnDisk()` + Save handler (`screen.cpp:2338`)                           | Save prompts the user before overwriting a file that another process has changed. Users should know this safety net exists.                             |
| **Missing-file save guard**          | `Action::Save` branch (`screen.cpp:2322`)                                                  | If the buffer's path no longer exists on disk, save asks for explicit confirmation. Worth a sentence.                                                   |
| **Long-running command kill prompt** | `Screen::runPipeCommand()` (`screen.cpp:1264`)                                             | After 10 s of inactivity an external formatter / shell pipe shows a "Kill? y/n" prompt. Used by Esc-f, Ctrl-p and Tab-format. Not mentioned anywhere.   |
| **X11 PRIMARY/CLIPBOARD sync**       | `Screen::syncPrimarySelection()` + cut/copy/paste paths (`screen.cpp:541`, `clipboard.*`)  | After every keystroke the active mark region is mirrored into PRIMARY (middle-click paste), and Cut/Copy publish to CLIPBOARD; Paste reads CLIPBOARD.   |
| **Auto-indent**                      | `Buffer::insertAt()` (`buffer.cpp:25`) — runs in `Cpp` **and** `Jsn` modes                 | README does not mention auto-indent at all, even though it is a major editing convenience. Note also: it is not C/C++-only as one might assume.         |
| **Conflict-marker handling**         | `Buffer::scanForConflicts()` + `ConflictMarker` token (`buffer.hpp:22`, `buffer.cpp:272`)  | Git merge conflict regions get distinct backgrounds. Ctrl-n/Ctrl-b walk between them. README hints at this in the shortcut row but doesn't explain it.  |
| **Block (hunk) navigation**          | `Action::NextBlock` / `PrevBlock` (`screen.cpp:2223`, `2247`)                              | Ctrl-n and Ctrl-b jump through `@@` hunk headers in diff buffers as well as conflict markers. README labels the keys but does not explain "what block". |
| **External clipboard read on paste** | `Action::Paste` (`screen.cpp:2402`)                                                        | Ctrl-y prefers X11 CLIPBOARD when it differs from the top of the internal ring, so external Ctrl-C from other apps becomes pasteable.                   |
| **Switch-buffer scratch syntax**     | `openSwitchBufferPrompt()` (`screen.cpp:830`)                                              | Typing a `*name*` value in the Ctrl-o prompt creates (or finds) a scratch buffer of that name. Not documented.                                          |
| **MRU buffer switching default**     | `BufferManager::secondMRU()`, used implicitly in switch prompt completions                 | The README says "MRU-ordered buffer switching" but does not say that Ctrl-o defaults to the previous buffer.                                            |
| **Modeline contents**                | `Screen::drawModeline()` (`screen.cpp:303`)                                                | The README never mentions that the bottom row shows `line:col`, prompt/status, `[mode]`, `+` for modified, and a history badge while browsing.          |
| **Terminal title**                   | `Screen::Screen()` (`screen.cpp:217`)                                                      | The terminal title is set to `cx@<cwd>` on startup. Worth a sentence so users aren't confused.                                                          |
| **Error log location**               | `error_log.cpp` writes `~/.cx-errors`                                                      | Architecture mentions `cx_error.log` (wrong name); README never says where errors and clipboard failures land.                                          |
| **Help / overlay 'q' dismissal**     | `Screen::handleNormalInput()` (`screen.cpp:1907`)                                          | `q` returns from any read-only overlay (Help, Buffer list, Paste ring) to the previous buffer. Help text mentions it; README does not.                  |
| **Esc-Esc dismisses overlays**       | `Screen::handleBareEscEsc()` (`screen.cpp:2033`)                                           | Esc-Esc dismisses an overlay first; only with no overlay does it clear the mark. README documents only the "clear mark" half.                           |
| **Esc-? toggles help**               | `Action::Help` (`screen.cpp:2278`)                                                         | Pressing Esc-? a second time returns to the underlying buffer.                                                                                          |
| **Path argument resolution**         | `main.cpp:67`, `BufferManager::open()`                                                     | A path argument that names a directory opens the directory browser; this is described in passing but not for filesystem disambiguation rules.           |
| **Stdin pipe → `*stdin*`**           | `main.cpp:46`                                                                              | The README does mention this, but only as a one-liner — it is not in the Features list.                                                                 |

### 1.2 Missing or incorrect entries in the **Keyboard Shortcuts** tables

| Issue                                     | Implementation                                                                                                                                  | Suggested fix                                                                                       |
|-------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------------|
| **History mode keys**                     | `[`, `]`, `Enter` (commit), `Esc-Esc` (cancel) are dispatched inline in `Screen::handleNormalInput` (`screen.cpp:1808`)                         | Add a "History mode" sub-table mirroring the help screen.                                           |
| **Esc-? to dismiss help**                 | `Action::Help` re-checks `prevBuf()` and returns                                                                                                | Note in the Files & Buffers row that pressing it again returns to editing.                          |
| **`q` to dismiss read-only overlays**     | `screen.cpp:1907`                                                                                                                               | Add `q` row under a new "Overlays" or "Files & Buffers" section.                                    |
| **Esc-Esc to dismiss overlay**            | `Screen::handleBareEscEsc()` (`screen.cpp:2037`)                                                                                                | Currently described only as "Clear mark"; should mention overlay dismissal first.                   |
| **Esc-Bksp**                              | Bound as a second alias to `DeleteChunkBefore` (`shortcut.cpp:35`)                                                                              | Add to the Deletion table.                                                                          |
| **Replace-prompt keys (`y`/`n`/`!`/`q`)** | `openReplaceConfirmPrompt()` (`screen.cpp:966`)                                                                                                 | The README describes the prompt verbally but the keys aren't in any table.                          |
| **Find-prompt Ctrl-f / Ctrl-r**           | `openFindPrompt()` `onSpecialKey` (`screen.cpp:901`)                                                                                            | While in the Find prompt, Ctrl-f advances and Ctrl-r reverses without leaving the prompt.           |
| **Set Mode prompt vocabulary**            | `openSetModePrompt()` (`screen.cpp:1026`) accepts `c`/`cpp`/`c++`, `jsn`/`json`, `md`/`markdown`, `txt`/`text`, `dif`/`diff`/`patch`            | README has only "Set buffer mode" — it should at least list the canonical names in a sentence.      |
| **Go-to-line accepts `last`**             | `openGoToLinePrompt()` (`screen.cpp:669`) treats the literal string `last` as "jump to end of buffer"                                           | Add to the Go-to-line description.                                                                  |
| **Up / Down arrow as prompt history**     | `handlePromptInput()` (`screen.cpp:572`)                                                                                                        | Per-prompt history navigation is implemented but undocumented.                                      |
| **Tab in prompts cycles completions**     | `Prompt::onTab` callbacks (e.g. `screen.cpp:758`)                                                                                               | Mention tab cycling.                                                                                |
| **Right arrow in completion prompts**     | `onRightArrow` (e.g. `screen.cpp:774`)                                                                                                          | Right arrow accepts the current completion and re-runs it (e.g. enters a directory). Undocumented.  |
| **Format key `Esc-f` is C/C++-only**      | `executeFormat()` (`screen.cpp:1441`) errors with "No formatter for this file type" otherwise                                                   | The shortcut table says "Format buffer/region (C/C++)" — that is fine; restriction confirmed.       |
| **Toggle-comment `Esc-;` is C/C++-only**  | `Action::ToggleComment` (`screen.cpp:2534`) explicitly limits the prefix to Cpp                                                                 | Listed as "Toggle line comment (C/C++)" — fine.                                                     |
| **`?` only in editable buffers**          | `screen.cpp:1822` gates on `!_buf->readonly()`                                                                                                  | The "Bracket matching" section says "every mode" — which is true for *modes* but false for overlays |
| **Mouse drag does NOT extend selection**  | `handleMouseEvent()` (`screen.cpp:1942`) ignores motion events                                                                                  | README says "left-click positions the cursor" — which is correct, but a future user may expect drag |

### 1.3 Wording corrections

- **"Smart find"** description in the README should clarify that `Ctrl-f`/`Ctrl-r` while the prompt is empty re-runs the previous search (the prompt opens with the previous pattern as a dim hint, and Enter on an empty prompt is a no-op). Currently somewhat implied.
- **Find** description says highlight is "temporary" — actually 3 s, governed by `kHighlightTtl` (`screen.cpp:110`). Either drop the timing or document it.
- **Status messages** age out after 10 s (`kStatusMessageTtl`). Not documented.

---

## 2. docs/architecture.md

### 2.1 Missing actions / API drift

| Item                            | Implementation                                                                                       | Doc fix                                                                                |
|---------------------------------|------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------|
| **`Action::DeleteLine`**        | `shortcut.hpp:13` — Ctrl-k                                                                           | The Action list at the end of the Shortcut section omits `DeleteLine`.                 |
| **`Esc-Bksp` chord**            | Second binding to `DeleteChunkBefore` (`shortcut.cpp:35`)                                            | Note this dual binding in the Shortcut section.                                        |
| **C-x prefix dispatch**         | `Screen::handleCtrlXInput()` (`screen.cpp:2055`) and `shortcutFromCxChar()` (`shortcut.hpp:82`)      | Architecture mentions "Ctrl-key path and Esc-chord path" but never C-x.                |
| **Modes accepted by Set Mode**  | `openSetModePrompt()` aliases (see 1.2)                                                              | Worth a sentence in the Screen section.                                                |
| **Help dismissal & overlays**   | `Action::Help` toggles the help buffer over a "prevBuf" stash (`screen.cpp:2278`)                    | Architecture should describe the "readonly overlay over a previous buffer" pattern.    |
| **`Panel::prevBuf`**            | `Panel` carries a per-panel `prevBuf` slot used by Help / Buffer-list / Paste-ring overlays          | Panel section in architecture doesn't mention it.                                      |
| **`Panel::saveViewState()`**    | Used by close-panel and buffer-switch flows                                                          | Mentioned briefly but should be tied to the buffer-switch / panel-close lifecycle.     |
| **Up to **6** panels**          | `FocusPanel1..6`, `splitFocused()` enforces 10×3 minimum                                             | Architecture doesn't state the max-panel limit; README does (good catch).              |

### 2.2 Missing classes / subsystems

| Subsystem                                  | What needs adding                                                                                                                                                                     |
|--------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **`LineEndingStyle` / `normalizeLineEndings()`** | `TextBuffer` exposes `lineEndingStyle()`, `acknowledgeLineEndingStyle()`, `normalizeLineEndings()` (`text_buffer.hpp:14`). Architecture's `TextBuffer` section never mentions any of these. |
| **`Buffer::cachedLineTokens()`**           | Mentioned in the syntax section but not in the `Buffer` section's "Caching" bullets.                                                                                                  |
| **`Buffer::scanForConflicts()`**           | Worth a one-line bullet in the `Buffer` section.                                                                                                                                      |
| **`requireNotInHistoryMode()`**            | A private helper used by every history-mode-incompatible action; worth mentioning so the failure-mode is greppable.                                                                   |
| **`Buffer::SavedViewState` and overlay restoration** | Architecture describes saved view state but does not connect it to the help/buffer-list/paste-ring overlay flow that uses `Panel::prevBuf`.                                       |
| **`bufferWorkDir()`**                      | Used by every subprocess (Format, Tab-format, Pipe-replace). Not mentioned.                                                                                                           |
| **`updateFocusPointers()`**                | Helper that re-binds `_buf` / `_view` after layout changes; worth a sentence in the Screen section since a reader looking at `_buf = _layout->focused().buffer()` will be confused.    |
| **`Buffer::_bracketCache`**                | The "Caching" bullets describe it correctly, but the surrounding text in the bracket-matching paragraph never explains why `cachedLineTokens()` exists.                               |

### 2.3 Wrong file paths / minor errors

| Where                     | What is wrong                                                                                                          |
|---------------------------|------------------------------------------------------------------------------------------------------------------------|
| `Clipboard` paragraph     | Says errors are logged to `~/.cx-errors` — correct (`error_log.cpp`).                                                  |
| Top-level error log line  | The supporting-types table says `error_log.hpp/.cpp` "appends diagnostic messages to `~/.cx-errors`" — also correct.   |
| **`cx_error.log`**        | The CLAUDE.md project guidelines (one level up) say `cx_error.log` — the actual file is `~/.cx-errors`. Conflict.      |
| `_synCache` / `_bracketCache` | Architecture mentions both as `Caching` items — accurate.                                                          |
| **`SyntaxToken` enum list** | Architecture's enum list mentions `Number`, `Preprocessor`, etc. — matches code. **Good.**                           |

### 2.4 Layout / structural

- The architecture document is mostly accurate at the class level but still organises the content in `main → Screen → BufferManager → Buffer*` tree style. Adding a small "Lifecycle / overlays" diagram showing the Help/BufferList/Dir/PasteRing overlay pattern (`prevBuf` chain) would help new contributors more than another paragraph.

---

## 3. docs/highlighting.md

### 3.1 Token Types table is incomplete

The table omits five tokens that are present in `SyntaxToken` (`modes/c/syntax.hpp`) and rendered by `Buffer::render()` (`buffer.cpp:373`):

| Missing token     | Modes      | Visual style                                                  |
|-------------------|------------|---------------------------------------------------------------|
| `DiffAdd`         | Diff       | Background `\x1b[48;2;30;60;30m` (dark green)                 |
| `DiffRemove`      | Diff       | Background `\x1b[48;2;70;30;30m` (dark red)                   |
| `DiffMeta`        | Diff       | Reverse video                                                 |
| `DiffHunk`        | Diff       | Foreground `\x1b[38;2;120;160;220m` (light blue)              |
| `ConflictMarker`  | All modes  | Per-marker background: ours `\x1b[48;2;40;50;80m`, divider `\x1b[48;2;70;60;30m`, theirs `\x1b[48;2;50;70;40m` |

These should appear both in the **Token Types** table and in the **Colors** table.

### 3.2 Multi-line state

The "Multi-line State" section lists three fields:

- `inBlockComment`
- `inPreprocessor`
- `rawDelim`

Verify against `modes/c/syntax.hpp` — the file does carry these three. However, the document does not mention:

- `advanceBlockCommentState()` — a cheaper variant of `tokenizeLine()` that only advances state, used by `findMatchingBracket()` to skip large stretches of source quickly.
- The fact that JSON, Markdown and Diff modes accept the `SyntaxState` parameter for interface consistency only and do not maintain any.

### 3.3 Mode-vs-token cross-reference

The table does not say which tokens **never** appear in which modes. A short matrix at the end (rows = tokens, columns = modes, `✓`/`—`) would be unambiguous and would have caught the diff-token omission.

### 3.4 Bracket-match colour

`Buffer::render()` uses `\x1b[38;2;60;170;170m` (turquoise) for the bracket on the cursor side **and** for its match. The doc lists this under "Bracket match" in the Colors table — accurate. It does not, however, document the *fallback* behaviour where the match colour interacts with ConflictMarker / DiffAdd backgrounds (the renderer restores the prior fg colour on transitions). Not strictly a defect but worth a footnote.

---

## 4. docs/potential_improvements.md

This file documents one item ("Dirty-region rendering"). Suggestions:

- Re-frame as a list and add at least the items already known but unimplemented:
  - **Storage scaling** — already discussed in `architecture.md` "Performance characteristics", but a TODO entry would consolidate the future-work pointer.
  - **Async clipboard reads** — `Clipboard::getClipboard()` is synchronous (architecture explicitly notes this); a non-blocking variant for large external pastes is a natural improvement.
  - **Subprocess timeout configurability** — the 10-second kill-prompt delay in `screen.cpp:114` is hardcoded.
  - **Per-buffer wrap / whitespace state** — `_wrap` and `_showWhitespace` are global on `Screen`; making them per-view is a natural improvement.

(These are *suggestions for that file's growth*, not undocumented features.)

---

## 5. Summary

- **README.md** is the most out-of-date of the three: it describes the editor as it was when only the basic feature set existed and has not absorbed the line-ending detection, disk-change guard, kill-prompt, X11 clipboard sync, modeline conventions, overlay dismissal, prompt-history navigation, completion cycling and `last` go-to-line shortcut.
- **docs/architecture.md** is accurate at the class level but missing several smaller surfaces (`DeleteLine`, C-x dispatch, `LineEndingStyle`, `prevBuf` overlays, `bufferWorkDir`).
- **docs/highlighting.md** is missing the entire diff- and conflict-marker token family, plus the cross-reference between modes and tokens.
- **docs/potential_improvements.md** is correct but minimal; it could absorb a handful of known unfinished items.

A single update commit could resolve roughly half of these by extending the existing tables; the remainder need new sentences or new sub-sections.
