# Regex dialect

Esc-X (regex find) and Esc-Y (regex replace) use this dialect. cx's own engine in `src/regex.hpp/.cpp` implements
it. The engine cannot backtrack. It replaced `std::regex` on 2026-11-18. Find-in-files (Esc-g) does not use it. It
runs `git grep` or `grep`, which have their own syntax.

## Goals

- No freezes. Matching a line of length *n* against a pattern of size *m* takes O(n·m) time and O(m) memory,
  whatever the pattern. Nothing recurses, so long lines cannot overflow the stack either.
- UTF-8 awareness. The unit of matching is a code point, not a byte.
- Familiar syntax. The dialect is a subset of ECMAScript/Perl syntax. Every pattern that it accepts means what it
  means in ECMAScript, with the exceptions in *Differences from ECMAScript*. The dialect rejects every feature that
  it leaves out, with a clear message, instead of reading it differently.
- Stdlib only, like the rest of cx.

## Scope of a match

- A match spans lines only where the pattern matches a line break. These match a line break: `\n`, a literal
  newline in the pattern (typed with Ctrl-q Enter, or pasted), `\s`, `[[:space:]]`, and a class that lists it
  (`[\n ]`). Nothing else matches one: not `.`, and not a negated class (`[^…]`, `\D`, `\W`, `\S`, `[\D]`). Negated
  classes do match tab and `\r`. (ECMAScript lets negated classes match a line break.)
- If a pattern cannot match a line break, cx matches it line by line. If it can, cx matches it over the buffer's
  lines as one text, with a line break between each two lines (`Regex::spansLines()`, `Regex::searchLines()`).
- `^` and `$` match at the start and end of every line.
- A search that starts mid-line still sees the text before the start position. So `^` matches only at a real line
  start, and `\b` sees the previous character.
- Matching is case-sensitive.

## Matching unit: code points

- `.`, a character class and a literal each match one code point.
- An invalid UTF-8 byte in the line is a unit of its own. `.` and negated classes (`[^a]`, `\D`, `\W`, `\S`) match
  it. No literal and no positive class matches it.
- Matches start and end on code-point boundaries, so a replacement can never cut a multi-byte character in half.
- The unit is the code point, not the grapheme cluster. So `.` matches the `e` of a decomposed `é` (`e` + U+0301)
  and leaves the combining mark alone. Every common engine does the same. When cx places the cursor on a match, the
  cursor already snaps to the start of the cluster.

## Syntax

### Literals

Any character other than the metacharacters `\ ^ $ . | ? * + ( ) [ {` matches itself. This includes non-ASCII
characters (`中`, `é`, `😀`). `]` and `}` are literal when they do not close anything.

### Escapes

| Escape | Meaning |
|---|---|
| `\` + any ASCII punctuation | that character literally (`\.`, `\*`, `\\`, `\/`, …) |
| `\n` | a line break |
| `\t`, `\r` | tab, carriage return (also `\f`, `\v`, for ECMAScript compatibility) |
| `\xHH` | the code point U+00HH |
| `\uHHHH`, `\u{H…}` | the code point U+HHHH (braces: 1 to 6 hex digits, up to U+10FFFF) |
| `\d` / `\D` | ASCII digit `[0-9]` / anything else but a line break |
| `\w` / `\W` | ASCII word character `[A-Za-z0-9_]` / anything else but a line break |
| `\s` / `\S` | ASCII whitespace (space, tab, line break, `\v`, `\f`, `\r`) / anything else but a line break |
| `\b` / `\B` | at a word boundary / not at one (word = `\w`) |

An escaped space or non-ASCII character is that character. Any other escape (of a letter, a digit or a control
character) is an error. In particular, `\1`…`\9` are *back-references* in ECMAScript. The dialect refuses them with
"back-references are not supported".

### Character classes

- The forms are `[abc]`, `[^abc]`, and ranges such as `[a-z]`, `[à-ÿ]` and `[一-龥]`. A range runs over code points.
  A range whose end comes before its start is an error.
- A class can contain `\d \w \s`, their negations, and the escapes above. `\b` inside a class is an error, because
  ECMAScript reads it as backspace.
- `-` is literal first or last (`[-a]`, `[a-]`). `]` is literal first (`[]a]`) or escaped. `^` is literal anywhere
  but first.
- POSIX classes inside brackets are ASCII only: `[[:alpha:]]`, `[:digit:]`, `[:alnum:]`, `[:upper:]`, `[:lower:]`,
  `[:space:]`, `[:punct:]`, `[:xdigit:]`, `[:word:]`. std::regex accepted these too, so habits from before the
  engine still work.

### Anchors

`^` matches at the start of the line, and `$` at its end. `\b` and `\B` (see above) are anchors too.

### Groups and alternation

- `(…)` is a capturing group. Groups are numbered by their opening parenthesis, from 1.
- `(?:…)` is a non-capturing group.
- `a|b` is alternation, with the lowest precedence. Empty alternatives are allowed (`a|`).
- The dialect refuses these with a clear message: lookahead and lookbehind (`(?=`, `(?!`, `(?<=`, `(?<!`), and
  named groups (`(?<name>…)`).

### Quantifiers

| Greedy | Lazy | Repeats the preceding item |
|---|---|---|
| `*` | `*?` | 0 or more times |
| `+` | `+?` | 1 or more times |
| `?` | `??` | 0 or 1 time |
| `{n}` | `{n}?` | exactly n times |
| `{n,}` | `{n,}?` | n or more times |
| `{n,m}` | `{n,m}?` | n to m times |

A greedy quantifier takes as many repetitions as still let the whole pattern match. A lazy one takes as few. For
example, on the line `<a><b>`:

- `<.*>` matches `<a><b>`, because the greedy `.*` runs to the last `>`.
- `<.*?>` matches `<a>`, because the lazy `.*?` stops at the first `>`.

Limits:

- Counts are at most 1000. A larger count is an error, as in RE2.
- A `{` that does not start a valid quantifier is an error. Write `\{` for a literal brace.
- A quantifier with nothing to repeat (`*a`, `a**`) is an error.
- Patterns compile to a program of limited size (for example 20,000 instructions). `{n,m}` copies its operand, so
  the dialect refuses `(a{1000}){1000}` as "pattern too large".

## Match semantics

- Leftmost: the match that starts earliest in the line wins.
- First by priority: among the matches at that start, the first one by priority wins, as in ECMAScript and Perl.
  Alternatives are tried left to right. Greedy quantifiers prefer more repetitions, and lazy ones prefer fewer.
  std::regex used the same order, so most patterns give the same matches as before the engine.
- Captures: a group inside a repetition reports its *last* iteration. An optional group that did not take part in
  the match is empty.
- Empty matches (`x*`, `^`, `\b`) behave as they did with std::regex. Find and skip step past them. Replace-all
  replaces one and then moves one code point on.

## Replacement templates (Esc-Y)

| Token | Inserts |
|---|---|
| `$&`, `$0` | the whole match |
| `$1` … `$9` | group 1…9, empty if the group did not take part in the match |
| `$$` | a literal `$` |
| `` $` `` / `$'` | the text of the line before / after the match |

- Any other `$` is inserted literally (`$x`, a trailing `$`).
- `$1` to `$9` take one digit only. `$10` is group 1 followed by `0`.
- `\n`, `\t`, `\r` and `\\` insert a line break, tab, carriage return and backslash. These are the characters that
  the same escapes match in a pattern. Any other backslash is literal (`\q`, `\1`, a trailing `\`).

## Errors

Compiling returns either the program or a message for the status line. The message names the problem and its
position, for example `regex: back-references are not supported (at 4)` or `regex: unmatched ')' (at 7)`. An
invalid pattern never searches.

## Interruption

The engine is linear, but a whole-buffer search on tens of megabytes can still take a second or two. So find, skip
and replace-all check for an interruption between lines, once per megabyte scanned. These interrupt a search:

- a waiting Esc or Ctrl-g
- an escape sequence, such as an arrow key, which cx reads whole
- a pending SIGTERM

On an interruption, the search stops. Find says "Search cancelled", and replace says "Replaced N occurrence(s);
cancelled". A cancelled replace-all keeps the lines that it already rewrote, as one history version. cx keeps any
other key typed during the search and acts on it after the search.

## Differences from ECMAScript (and from cx before this engine)

| ECMAScript / std::regex | This dialect |
|---|---|
| back-references `\1` in the pattern | error |
| lookahead `(?=…)`, `(?!…)` | error |
| byte-wise: `.` can match one byte of `é` | code points |
| a group in a repetition is reset each iteration (`((a)\|b)+` on `ab` leaves `$2` empty) | keeps its last iteration (`$2` = `a`), as Perl and RE2 |
| `\b` in a class is backspace | error |
| `[]` is an empty class, `[^]` any character | `]` first in a class is literal: `[]a]` is `]` or `a` |
| a negated class or `\D`/`\W` matches a line break | never |
| `^` / `$` at line breaks only with the `m` flag | always at every line start / end |
| the replacement takes no backslash escapes | `\n \t \r \\` |
| unknown escapes such as `\q` mean `q` | error |
| unlimited `{n,m}` | n, m ≤ 1000 |
| exponential / quadratic time on some patterns | O(n·m) |

## Implementation

- `src/regex.hpp/.cpp` has the class `Regex`:
  - `static std::expected<Regex, std::string> compile(std::string_view)`
  - `search(line, from)` returns a `std::optional<Match>`, with the group spans as byte offsets.
  - `matchAt(line, from)` is an anchored match, for replacement.
  - `expand(template, line, match)` builds the replacement text.
- The parser uses recursive descent over the pattern and builds an AST. A cap on group nesting (say 100) limits the
  recursion depth.
- The compiler turns the AST into Pike VM instructions:
  - `Char`, `Class` (sorted code-point ranges), `Any`
  - `Split` (with priority), `Jmp`, `Save`
  - `AssertBol`, `AssertEol`, `AssertWordB`, `AssertNotWordB`
  - `Match`
- Text adapters: the VM runs over one line (`search`, `matchAt`) or over a `LineSource` (`searchLines`,
  `matchAtLines`). A `LineSource` presents lines as one text, with a line break between each two lines. Positions
  are byte offsets into that text, and cx turns them back into points at the end. Only patterns that can match a
  line break take this second path.
- The Pike VM makes one pass over the line, one code point per step. It keeps thread lists in priority order and
  capture slots for each thread. An unanchored search adds a fresh thread at each position until it finds the first
  match.
- Literal shortcuts: every match starts with some literal text (through groups and zero-width assertions). The
  search uses `find()` to jump to the occurrences of that text. A longer literal that every match contains rejects
  any line that lacks it. The remaining cases run at roughly 50 to 80 MB/s.
- `regex_search.hpp`: `findFirstRegex`, `findLastRegex`, `findNextRegexInBuffer`, `substituteCaptures` and
  `replaceAllRegexForward` take a `Regex`. Before the engine, they took a `std::regex`. Their behaviour did not
  change: context before the start, empty-match stepping, and one history version per replace-all.
- Tests (`tests/test_regex.cpp`):
  - unit tests for each syntax element and error message
  - the regex tests from before the engine, adjusted where this spec differs
  - linear-time tests (`pathological_patterns_take_linear_time`): patterns that `parse_fuzz` reported as `HANG`
    with std::regex finish in milliseconds on 200 KB lines
- Fuzzing (`tools/fuzz/regex_fuzz.cpp`):
  - A differential harness compares the engine against `std::regex`. It uses patterns inside the shared subset, on
    ASCII lines where bytes and code points agree, and does not compare captures inside repetitions.
  - `searchLines()` on random lines must find what `search()` finds in the same lines joined by `\n`.
  - Random patterns must either compile or be refused. They must never crash, and a match must lie on code-point
    boundaries.
  - Random patterns on long lines must stay fast. The harness reports any search slower than `SLOW_MS` (2000 ms by
    default).
- Docs: the README has a section on the dialect, the in-line help (Esc-?) has the section below, and
  `docs/architecture.md` describes the `Regex` class. The README no longer has its "Known limitation" paragraph
  about std::regex.

## In-line help section

This section is at the end of the left column of `HelpBuffer` (`src/buffer/help_buffer.cpp`), after "Prompts:". It
uses the same layout as the rest: a key in 16 columns, then the description. `formatRawLine()` builds it, since none
of it is a shortcut. Keys are at most 13 characters, so a space always separates a key from its description:

```
Regex (Esc-X, Esc-Y):
                Case-sensitive; spans lines only
                where \n, \s or [\n] match one
  .             Any character but a line break
  [abc] [^abc]  One of / none of the listed
  [a-z]         A range of characters
  \d \D         Digit 0-9 / anything else
  \w \W         Letter, digit or _ (ASCII) / else
  \s \S         Space, tab, line break... / else
  [[:alpha:]]   POSIX class (ASCII); also digit
                alnum upper lower space punct
                xdigit word
  ^ $           Start / end of a line
  \b \B         At a word boundary / not at one
  x*            x 0+ times, as many as fit
  x+            x 1+ times, as many as fit
  x?            x 0 or 1 time, 1 if it fits
  x{n} x{n,}    x n times / n or more times
  x{n,m}        x n to m times, as many as fit
  x*? x+? x??   Lazy: as few times as possible
  x{n,m}?       Lazy: n to m, as few as possible
  a|b           a or b
  (x)           Group, captured as $1..$9
  (?:x)         Group, not captured
  \. \* \\ ...  Punctuation taken literally
  \n \t \r      Line break, tab, carriage return
  \xHH \uHHHH   Character by hex code point
  \u{10FFFF}    Character by code point, 1-6 digits
  $& or $0      Replacement: the whole match
  $1 .. $9      Replacement: group 1..9
  $$ $` $'      Replacement: $ / text before / after
  \n \t \r \\   Replacement: those characters
  Unsupported   \1 in a pattern, (?= (?! (?<= (?<!
```

"As many as fit" means greedy: as many repetitions as still let the rest of the pattern match. The README has the
`<.*>` / `<.*?>` example.

## Decisions

1. Lazy quantifiers: included. They cost almost nothing in a Pike VM, because the two branches of a `Split` only
   swap priority.
2. Case-insensitive matching: not now. A leading `(?i)` with ASCII case folding could come later.
3. Unicode classes: no. `\w`, `\d`, `\s` and `\b` stay ASCII, as in ECMAScript without the `u` flag.
4. Group references in replacements: a single digit only. `$10` is group 1 followed by `0`.
