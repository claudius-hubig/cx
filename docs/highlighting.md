# Syntax highlighting reference

## Token types

| Token          | Description                                        | Modes                          |
|----------------|----------------------------------------------------|--------------------------------|
| Normal         | Default text, digits, whitespace                   | All                            |
| Keyword        | Language keywords (`if`, `for`, ...), and YAML/TOML keys | C/C++, Shell, YAML, TOML, Py |
| Comment        | Line and block comments (`//`, `/* */`, `#`)       | C/C++, Shell, YAML, TOML, Py   |
| String         | `"..."`, `'...'`, raw / triple / heredoc strings   | C/C++, JSON, Shell, YAML, TOML, Py |
| Operator       | `+`, `-`, `=`, `&&`, `<<`, `:`, `,`, pipes etc.    | C/C++, Shell, YAML, TOML, Py   |
| Preprocessor   | `#` directives, YAML directives/anchors/tags, TOML table names, `@decorator` | C/C++, YAML, TOML, Py |
| Paren          | `(` `)`                                            | C/C++, Shell, Py               |
| Bracket        | `[` `]`                                            | C/C++, JSON, Shell, YAML, TOML, Py |
| Brace          | `{` `}`                                            | C/C++, JSON, Shell, YAML, TOML, Py |
| Angle          | `<` `>` when used as template brackets             | C/C++                          |
| Heading        | `# ...` headings                                   | Markdown                       |
| Bold           | `**...**` bold text                                | Markdown                       |
| Link           | `[text](url)` links                                | Markdown                       |
| Number         | Value literals: numbers and `true`/`false`/`null`/`None`/`nullptr` | C/C++, JSON, YAML, TOML, Py |
| Variable       | `$VAR`, `${...}`, `$?` parameter expansions        | Shell                          |
| DiffAdd        | Whole `+` line                                     | Diff                           |
| DiffRemove     | Whole `-` line                                     | Diff                           |
| DiffMeta       | `+++` / `---` file headers                         | Diff                           |
| DiffHunk       | `@@ ... @@` hunk headers                           | Diff                           |
| ConflictMarker | `<<<<<<<`, `=======`, `>>>>>>>` (any 7-char run)   | All                            |

Numbers and the boolean and null constants (`true`/`false`/`null`/`None`/`nullptr`/YAML `yes`/`no`/`~`/…) are all
*literal values*. They share one token (`Number`) and one colour. That colour is deliberately different from the
`Keyword` colour of language keywords (`if`, `for`, `class`, …) and from the `String` colour of string literals.
Shell has no syntax for literal values, so it emits neither `Number` nor those constants. Its `true` and `false` are
ordinary commands.

## Colours (24-bit RGB)

| Token         | RGB             | Colour name     | ANSI escape                |
|---------------|-----------------|-----------------|----------------------------|
| Keyword       | (80, 130, 200)  | Blue            | `\x1b[38;2;80;130;200m`    |
| Comment       | (80, 160, 80)   | Green           | `\x1b[38;2;80;160;80m`     |
| String        | (200, 80, 80)   | Red             | `\x1b[38;2;200;80;80m`     |
| Operator      | (150, 100, 200) | Purple          | `\x1b[38;2;150;100;200m`   |
| Number        | (100, 200, 200) | Light turquoise | `\x1b[38;2;100;200;200m`   |
| Preprocessor  | (180, 140, 60)  | Amber           | `\x1b[38;2;180;140;60m`    |
| Variable      | (210, 150, 90)  | Orange          | `\x1b[38;2;210;150;90m`    |
| Heading       | (80, 130, 200)  | Bold + Blue     | `\x1b[1;38;2;80;130;200m`  |
| Bold          | none            | Bold (no colour)| `\x1b[1m`                  |
| Link          | (100, 180, 180) | Teal            | `\x1b[38;2;100;180;180m`   |
| Bracket match | (60, 170, 170)  | Turquoise       | `\x1b[38;2;60;170;170m`    |

Paren, Bracket, Brace and Angle tokens use the default foreground colour.

Diff and conflict tokens are background colours, and the foreground stays the default text colour. So the syntax
of the changed line stays readable:

| Token             | RGB             | Colour name       | ANSI escape                |
|-------------------|-----------------|-------------------|----------------------------|
| DiffAdd (bg)      | (30, 60, 30)    | Dark green        | `\x1b[48;2;30;60;30m`      |
| DiffRemove (bg)   | (70, 30, 30)    | Dark red          | `\x1b[48;2;70;30;30m`      |
| DiffMeta          | none            | Reverse video     | `\x1b[7m`                  |
| DiffHunk (fg)     | (120, 160, 220) | Light blue        | `\x1b[38;2;120;160;220m`   |
| Conflict ours     | (40, 50, 80)    | Dark blue (bg)    | `\x1b[48;2;40;50;80m`      |
| Conflict divider  | (70, 60, 30)    | Dark amber (bg)   | `\x1b[48;2;70;60;30m`      |
| Conflict theirs   | (50, 70, 40)    | Dark olive (bg)   | `\x1b[48;2;50;70;40m`      |
| Current line      | (38, 38, 48)    | Dim blue-grey (bg)| `\x1b[48;2;38;38;48m`      |

The current-line tint is off by default. `Esc-_` toggles it for each view. It applies only to lines that do not
already have a diff or conflict background.

A bracket match can change the foreground on a line that has a diff or conflict background. After the match, the
renderer restores the previous foreground, not the global default. So the line's background stays the same across
the match.

## Available colours

These colours are still unused and are readable on both dark and light backgrounds:

| RGB             | Colour name    | Notes                                |
|-----------------|----------------|--------------------------------------|
| (200, 150, 80)  | Gold           | Warmer and brighter than amber       |
| (180, 80, 150)  | Rose / Magenta | Different from both red and purple   |
| (200, 120, 100) | Coral          | Between red and orange               |
| (130, 180, 80)  | Lime           | Between green and yellow             |
| (160, 130, 180) | Lavender       | Lighter than purple, and different from it |

## Multi-line state

The `SyntaxState` struct carries state from one line to the next:

- `inBlockComment`: inside `/* ... */` (C/C++).
- `inPreprocessor`: the line before was a `#` directive that ended with `\` (C/C++).
- `rawDelim`: inside `R"delim(...)delim"`. It holds the delimiter string (C/C++).
- `heredocTag` / `heredocStripTabs`: inside a shell heredoc body, until a line equal to the tag. The `StripTabs`
  flag marks the `<<-` form.
- `pyTriple` / `pyTripleRaw`: inside a Python `'''`/`"""` string. `Raw` turns off backslash-escape handling for
  `r'''...'''`.
- `yamlBlockCol`: inside a YAML `|`/`>` block scalar that starts at this indent column (`-1` means not in one).
- `tomlTriple` / `tomlDepth`: `tomlTriple` is the quote character of an open TOML `'''`/`"""` string (`0` means
  not in one). `tomlDepth` is the number of `[`/`{` that earlier lines left open (multi-line arrays).
- `mdLink`: a Markdown `[...](...)` link that continues past the end of the line (`None` / `Label` / `Url`). A
  blank line, a fenced code block, a list item or a table row ends an open link. So an unmatched `[` colours at most
  the rest of its paragraph or list item.
- `table`: whether the line above was the delimiter row of a table or a row below it. Otherwise it holds the cell
  count of the line above, which may be a header. In a table row, a link must close on that row.
- `fence`: inside a Markdown fenced code block. It holds the fence character and the length of the opening run.
  Those lines, and the fence lines themselves, get no markup tokens.

The C/C++, Shell, Python, YAML, TOML and Markdown tokenizers each keep multi-line state. Only the JSON and Diff
tokenizers have no state. They take the `SyntaxState` parameter so that all tokenizers have the same interface, but
they do not read or write it.

`advanceBlockCommentState()` is a cheaper companion. Callers use it to move the state forward across many lines
without making token vectors, for example when `findMatchingBracket()` skips to a distant match. It skips the
identifier and keyword classification, the bracket tagging and the token allocation for each byte. It only tracks
multi-line block comments and raw string literals.

## Token and mode coverage

Each mode emits only some of the tokens. A token that a mode does not produce shows as `Normal`.

| Token          | Cpp | Md | Jsn | Dif | Sh | Yml | Tml | Py | Txt |
|----------------|-----|----|-----|-----|----|-----|-----|----|-----|
| Normal         |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |
| Keyword        |  ✓  |    |     |     | ✓  |  ✓  |  ✓  | ✓  |     |
| Comment        |  ✓  |    |     |     | ✓  |  ✓  |  ✓  | ✓  |     |
| String         |  ✓  |    |  ✓  |     | ✓  |  ✓  |  ✓  | ✓  |     |
| Operator       |  ✓  |    |     |     | ✓  |  ✓  |  ✓  | ✓  |     |
| Preprocessor   |  ✓  |    |     |     |    |  ✓  |  ✓  | ✓  |     |
| Paren          |  ✓  |    |     |     | ✓  |     |     | ✓  |     |
| Bracket        |  ✓  |    |  ✓  |     | ✓  |  ✓  |  ✓  | ✓  |     |
| Brace          |  ✓  |    |  ✓  |     | ✓  |  ✓  |  ✓  | ✓  |     |
| Angle          |  ✓  |    |     |     |    |     |     |    |     |
| Heading        |     | ✓  |     |     |    |     |     |    |     |
| Bold           |     | ✓  |     |     |    |     |     |    |     |
| Link           |     | ✓  |     |     |    |     |     |    |     |
| Number         |  ✓  |    |  ✓  |     |    |  ✓  |  ✓  | ✓  |     |
| Variable       |     |    |     |     | ✓  |     |     |    |     |
| DiffAdd        |     |    |     |  ✓  |    |     |     |    |     |
| DiffRemove     |     |    |     |  ✓  |    |     |     |    |     |
| DiffMeta       |     |    |     |  ✓  |    |     |     |    |     |
| DiffHunk       |     |    |     |  ✓  |    |     |     |    |     |
| ConflictMarker |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |  ✓  | ✓  |  ✓  |
