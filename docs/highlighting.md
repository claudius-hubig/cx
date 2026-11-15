# Syntax Highlighting Reference

## Token Types

| Token          | Description                                        | Modes                          |
|----------------|----------------------------------------------------|--------------------------------|
| Normal         | Default text, digits, whitespace                   | All                            |
| Keyword        | Language keywords (`if`, `for`, ...); YAML/TOML keys | C/C++, Shell, YAML, TOML, Py |
| Comment        | Line and block comments (`//`, `/* */`, `#`)       | C/C++, Shell, YAML, TOML, Py   |
| String         | `"..."`, `'...'`, raw / triple / heredoc strings   | C/C++, JSON, Shell, YAML, TOML, Py |
| Operator       | `+`, `-`, `=`, `&&`, `<<`, `:`, `,`, pipes etc.    | C/C++, Shell, YAML, TOML, Py   |
| Preprocessor   | `#`-directives; YAML directives/anchors/tags; TOML table names; `@decorator` | C/C++, YAML, TOML, Py |
| Paren          | `(` `)`                                            | C/C++, Shell, Py               |
| Bracket        | `[` `]`                                            | C/C++, JSON, Shell, YAML, TOML, Py |
| Brace          | `{` `}`                                            | C/C++, JSON, Shell, YAML, TOML, Py |
| Angle          | `<` `>` when used as template brackets             | C/C++                          |
| Heading        | `# ...` headings                                   | Markdown                       |
| Bold           | `**...**` bold text                                | Markdown                       |
| Link           | `[text](url)` links                                | Markdown                       |
| Number         | Value literals: numbers + `true`/`false`/`null`/`None`/`nullptr` | C/C++, JSON, YAML, TOML, Py |
| Variable       | `$VAR`, `${...}`, `$?` parameter expansions        | Shell                          |
| DiffAdd        | Whole `+` line                                     | Diff                           |
| DiffRemove     | Whole `-` line                                     | Diff                           |
| DiffMeta       | `+++` / `---` file headers                         | Diff                           |
| DiffHunk       | `@@ ... @@` hunk headers                           | Diff                           |
| ConflictMarker | `<<<<<<<`, `=======`, `>>>>>>>` (any 7-char run)   | All                            |

Numbers and the boolean/null constants (`true`/`false`/`null`/`None`/
`nullptr`/YAML `yes`/`no`/`~`/…) are all *literal values* and share a
single token (`Number`) and colour, deliberately distinct from language
keywords (`if`, `for`, `class`, …) which keep the `Keyword` colour, and
from string literals which keep their own `String` colour. Shell has no
literal-value syntax, so it emits neither `Number` nor those constants
(its `true`/`false` are ordinary commands).

## Colors (24-bit RGB)

| Token         | RGB             | Color Name      | ANSI Escape                |
|---------------|-----------------|-----------------|----------------------------|
| Keyword       | (80, 130, 200)  | Blue            | `\x1b[38;2;80;130;200m`    |
| Comment       | (80, 160, 80)   | Green           | `\x1b[38;2;80;160;80m`     |
| String        | (200, 80, 80)   | Red             | `\x1b[38;2;200;80;80m`     |
| Operator      | (150, 100, 200) | Purple          | `\x1b[38;2;150;100;200m`   |
| Number        | (100, 200, 200) | Light turquoise | `\x1b[38;2;100;200;200m`   |
| Preprocessor  | (180, 140, 60)  | Amber           | `\x1b[38;2;180;140;60m`    |
| Variable      | (210, 150, 90)  | Orange          | `\x1b[38;2;210;150;90m`    |
| Heading       | (80, 130, 200)  | Bold + Blue     | `\x1b[1;38;2;80;130;200m`  |
| Bold          | —               | Bold (no color) | `\x1b[1m`                  |
| Link          | (100, 180, 180) | Teal            | `\x1b[38;2;100;180;180m`   |
| Bracket match | (60, 170, 170)  | Turquoise       | `\x1b[38;2;60;170;170m`    |

Paren, Bracket, Brace, and Angle tokens use the default foreground color.

Diff and conflict tokens are background colours (the foreground stays
the default text colour) so the underlying syntax of the changed line
remains readable:

| Token             | RGB             | Color Name        | ANSI Escape                |
|-------------------|-----------------|-------------------|----------------------------|
| DiffAdd (bg)      | (30, 60, 30)    | Dark green        | `\x1b[48;2;30;60;30m`      |
| DiffRemove (bg)   | (70, 30, 30)    | Dark red          | `\x1b[48;2;70;30;30m`      |
| DiffMeta          | —               | Reverse video     | `\x1b[7m`                  |
| DiffHunk (fg)     | (120, 160, 220) | Light blue        | `\x1b[38;2;120;160;220m`   |
| Conflict ours     | (40, 50, 80)    | Dark blue (bg)    | `\x1b[48;2;40;50;80m`      |
| Conflict divider  | (70, 60, 30)    | Dark amber (bg)   | `\x1b[48;2;70;60;30m`      |
| Conflict theirs   | (50, 70, 40)    | Dark olive (bg)   | `\x1b[48;2;50;70;40m`      |
| Current line      | (38, 38, 48)    | Dim blue-grey (bg)| `\x1b[48;2;38;38;48m`      |

The current-line tint is opt-in (per-view, toggled by `Esc-_`) and only
applies to lines that don't already carry a diff or conflict
background.

When a bracket-match foreground transition happens on a line that
already carries a diff/conflict background, the renderer restores the
previous foreground (not the global default) on the way out so the
line's background stays consistent across the match.

## Available Colors

Colors that are still unused and would be readable on both dark and light backgrounds:

| RGB             | Color Name     | Notes                                |
|-----------------|----------------|--------------------------------------|
| (200, 150, 80)  | Gold           | Warmer, brighter than amber          |
| (180, 80, 150)  | Rose / Magenta | Distinct from both red and purple    |
| (200, 120, 100) | Coral          | Between red and orange               |
| (130, 180, 80)  | Lime           | Between green and yellow             |
| (160, 130, 180) | Lavender       | Lighter purple, distinct from purple |

## Multi-line State

The `SyntaxState` struct carries state between lines:

- **inBlockComment** — inside `/* ... */` (C/C++)
- **inPreprocessor** — prior line was a `#`-directive ending with `\` (C/C++)
- **rawDelim** — inside `R"delim(...)delim"`, holds the delimiter string (C/C++)
- **heredocTag** / **heredocStripTabs** — inside a shell heredoc body
  until a line equal to the tag; the `StripTabs` flag is the `<<-` form
- **pyTriple** / **pyTripleRaw** — inside a Python `'''`/`"""` string;
  `Raw` suppresses backslash-escape handling for `r'''...'''`
- **yamlBlockCol** — inside a YAML `|`/`>` block scalar introduced at
  the given indent column (`-1` = not in one)
- **tomlTriple** / **tomlDepth** — inside a TOML `'''`/`"""` string
  (the quote char, `0` = not in one); number of `[`/`{` left open by
  earlier lines (multi-line arrays)
- **mdLink** — a Markdown `[...](...)` link spanning the line boundary
  (`None` / `Label` / `Url`); a blank line ends an open one, so an
  unmatched `[` colours the rest of its paragraph at most

C/C++, Shell, Python, YAML, TOML and Markdown tokenisation each maintain
multi-line state. Only the JSON and Diff tokenizers are stateless: they
accept the `SyntaxState` parameter for interface consistency but read
and write nothing through it.

A cheaper companion `advanceBlockCommentState()` is provided for
callers that need to fast-forward the state across many lines without
producing token vectors (e.g. `findMatchingBracket()` skipping to a
distant match). It skips identifier/keyword classification, bracket
tagging and the per-byte token allocation, leaving only the multi-line
block-comment / raw-string-literal tracking.

## Token / mode coverage matrix

A given mode emits only a subset of the token set. Tokens not produced
by a mode are reported as `Normal`.

| Token          | Cpp | Md | Jsn | Dif | Sh | Yml | Tml | Py | Txt |
|----------------|-----|----|-----|-----|----|-----|-----|----|-----|
| Normal         |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |
| Keyword        |  ✓  | —  |  —  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| Comment        |  ✓  | —  |  —  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| String         |  ✓  | —  |  ✓  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| Operator       |  ✓  | —  |  —  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| Preprocessor   |  ✓  | —  |  —  |  —  | —  |  ✓  |  ✓  | ✓  |  —  |
| Paren          |  ✓  | —  |  —  |  —  | ✓  |  —  |  —  | ✓  |  —  |
| Bracket        |  ✓  | —  |  ✓  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| Brace          |  ✓  | —  |  ✓  |  —  | ✓  |  ✓  |  ✓  | ✓  |  —  |
| Angle          |  ✓  | —  |  —  |  —  | —  |  —  |  —  | —  |  —  |
| Heading        |  —  | ✓  |  —  |  —  | —  |  —  |  —  | —  |  —  |
| Bold           |  —  | ✓  |  —  |  —  | —  |  —  |  —  | —  |  —  |
| Link           |  —  | ✓  |  —  |  —  | —  |  —  |  —  | —  |  —  |
| Number         |  ✓  | —  |  ✓  |  —  | —  |  ✓  |  ✓  | ✓  |  —  |
| Variable       |  —  | —  |  —  |  —  | ✓  |  —  |  —  | —  |  —  |
| DiffAdd        |  —  | —  |  —  |  ✓  | —  |  —  |  —  | —  |  —  |
| DiffRemove     |  —  | —  |  —  |  ✓  | —  |  —  |  —  | —  |  —  |
| DiffMeta       |  —  | —  |  —  |  ✓  | —  |  —  |  —  | —  |  —  |
| DiffHunk       |  —  | —  |  —  |  ✓  | —  |  —  |  —  | —  |  —  |
| ConflictMarker |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |
