# Regex dialect (draft spec)

Status: implemented (2026-11-18) in `src/regex.hpp/.cpp`. Replaces `std::regex` in Esc-X (regex find) and Esc-Y (regex
replace) with an engine of our own that cannot backtrack. Find-in-files (Esc-g) is not
affected: it runs `git grep` / `grep`, which have their own syntax.

## Goals

- **No freezes.** Matching a line of length *n* against a pattern of size *m* takes
  O(n·m) time and O(m) memory, whatever the pattern. Nothing recurses, so long lines cannot
  overflow the stack either.
- **UTF-8 aware.** The unit of matching is a code point, not a byte.
- **Familiar syntax.** A subset of ECMAScript/Perl syntax. Every pattern this dialect accepts
  means what it means in ECMAScript (with the exceptions listed under *Differences from
  ECMAScript*). Every feature it leaves out is rejected with a clear message instead of
  being read differently.
- **Stdlib only**, as the rest of cx.

## Scope of a match

- A match spans lines only where the pattern matches a line break. A line break is
  matched by `\n` (or a literal newline in the pattern, typed with Ctrl-q Enter or
  pasted), by `\s` and `[[:space:]]`, and by a class that lists it (`[\n ]`). Nothing else
  matches one: not `.`, and not a negated class (`[^…]`, `\D`, `\W`, `\S`, `[\D]`), which
  do match tab and `\r`. (ECMAScript lets negated classes match a line break.)
- A pattern that cannot match a line break is matched line by line; one that can is
  matched over the buffer's lines as one text, a line break between each two
  (`Regex::spansLines()`, `Regex::searchLines()`).
- `^` and `$` match at the start and end of every line.
- A search that starts mid-line still sees the text before the start position, so `^` only
  matches at a real line start and `\b` sees the previous character.
- Matching is case-sensitive.

## Matching unit: code points

- `.`, a character class and a literal each match one code point.
- An invalid UTF-8 byte in the line is a unit of its own. `.` and negated classes (`[^a]`,
  `\D`, `\W`, `\S`) match it; no literal and no positive class does.
- Matches start and end on code-point boundaries, so a replacement can never cut a
  multi-byte character in half.
- Code points, not grapheme clusters: `.` matches the `e` of a decomposed `é` (`e` +
  U+0301) and leaves the combining mark alone. That is what every common engine does. The
  cursor already snaps to cluster starts when it is placed on a match.

## Syntax

### Literals

Any character other than the metacharacters `\ ^ $ . | ? * + ( ) [ {` matches itself,
non-ASCII characters included (`中`, `é`, `😀`). `]` and `}` are literal when they do not
close anything.

### Escapes

| Escape | Meaning |
|---|---|
| `\` + any ASCII punctuation | that character literally (`\.`, `\*`, `\\`, `\/`, …) |
| `\n` | a line break |
| `\t`, `\r` | tab, carriage return (also `\f`, `\v`, for ECMAScript compatibility) |
| `\xHH` | the code point U+00HH |
| `\uHHHH`, `\u{H…}` | the code point U+HHHH (braces: 1–6 hex digits, up to U+10FFFF) |
| `\d` / `\D` | ASCII digit `[0-9]` / anything else but a line break |
| `\w` / `\W` | ASCII word character `[A-Za-z0-9_]` / anything else but a line break |
| `\s` / `\S` | ASCII whitespace (space, tab, line break, `\v`, `\f`, `\r`) / anything else but a line break |
| `\b` / `\B` | at a word boundary / not at one (word = `\w`) |

An escaped space or non-ASCII character is that character. Any other escape (of a
letter, a digit or a control character) is an error. In particular, `\1`…`\9` are
*back-references* in ECMAScript and are refused with "back-references are not supported".

### Character classes

- `[abc]`, `[^abc]`, ranges `[a-z]`, `[à-ÿ]`, `[一-龥]`. A range runs over code points; one whose end
  comes before its start is an error.
- A class can contain `\d \w \s`, their negations, and the escapes above. `\b` inside a
  class is an error (ECMAScript reads it as backspace).
- `-` is literal first or last (`[-a]`, `[a-]`); `]` is literal first (`[]a]`) or escaped;
  `^` is literal anywhere but first.
- POSIX classes inside brackets, ASCII only: `[[:alpha:]]`, `[:digit:]`, `[:alnum:]`,
  `[:upper:]`, `[:lower:]`, `[:space:]`, `[:punct:]`, `[:xdigit:]`, `[:word:]`. std::regex
  accepts these today, so existing habits keep working.

### Anchors

`^` matches at the start of the line, `$` at its end. Plus `\b` and `\B` from above.

### Groups and alternation

- `(…)` is a capturing group, numbered by its opening parenthesis from 1.
- `(?:…)` is a non-capturing group.
- `a|b` is alternation, with the lowest precedence. Empty alternatives are allowed (`a|`).
- Refused with a clear message: lookahead and lookbehind (`(?=`, `(?!`, `(?<=`, `(?<!`)
  and named groups (`(?<name>…)`).

### Quantifiers

| Greedy | Lazy | Repeats the preceding item |
|---|---|---|
| `*` | `*?` | 0 or more times |
| `+` | `+?` | 1 or more times |
| `?` | `??` | 0 or 1 time |
| `{n}` | `{n}?` | exactly n times |
| `{n,}` | `{n,}?` | n or more times |
| `{n,m}` | `{n,m}?` | n to m times |

A greedy quantifier takes as many repetitions as still let the whole pattern match; a lazy
one takes as few. Example on the line `<a><b>`:

- `<.*>` matches `<a><b>` (the greedy `.*` runs to the last `>`);
- `<.*?>` matches `<a>` (the lazy `.*?` stops at the first `>`).

Limits:

- Counts are at most 1000; a larger count is an error, as in RE2.
- `{` that does not start a valid quantifier is an error. Write `\{` for a literal brace.
- A quantifier with nothing to repeat (`*a`, `a**`) is an error.
- Patterns compile to a bounded program (for example 20,000 instructions). `{n,m}`
  copies its operand, so `(a{1000}){1000}` is refused as "pattern too large".

## Match semantics

- **Leftmost.** The match that starts earliest in the line wins.
- **First by priority** among matches at that start (as ECMAScript and Perl): alternatives
  are tried left to right, greedy quantifiers prefer more repetitions, lazy ones fewer.
  This is the order std::regex uses today, so most patterns give the same matches as now.
- **Captures.** A group inside a repetition reports its *last* iteration. An optional group
  that did not take part in the match is empty.
- **Empty matches** (`x*`, `^`, `\b`) behave as now. Find and skip step past them. Replace-all
  replaces one and moves one code point on.

## Replacement templates (Esc-Y)

| Token | Inserts |
|---|---|
| `$&`, `$0` | the whole match |
| `$1` … `$9` | group 1…9; empty if the group did not take part in the match |
| `$$` | a literal `$` |
| `` $` `` / `$'` | the text of the line before / after the match |

- Any other `$` is inserted literally (`$x`, a trailing `$`).
- `$1` through `$9` take one digit only: `$10` is group 1 followed by `0`.
- `\n`, `\t`, `\r` and `\\` insert a line break, tab, carriage return and backslash: the
  characters these escapes match in a pattern. Any other backslash is literal (`\q`, `\1`,
  a trailing `\`).

## Errors

Compiling returns either the program or a message for the status line naming the problem
and its position, for example `regex: back-references are not supported (at 4)` or
`regex: unmatched ')' (at 7)`. An invalid pattern never searches.

## Interruption

The engine is linear, but a whole-buffer search on tens of megabytes can still take a
second or two. Find, skip and replace-all check between lines, once per megabyte scanned,
for a waiting Esc or Ctrl-g (an escape sequence, such as an arrow key, cancels too and is
read whole) or a pending SIGTERM. On one they stop: find says "Search cancelled", the
replace prompts "Replaced N occurrence(s); cancelled". A cancelled replace-all keeps the
lines it already rewrote, as one history version. Any other key typed meanwhile is kept
and acted on after the search.

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

## Implementation outline

- **`src/regex.hpp/.cpp`**, class `Regex`:
  - `static std::expected<Regex, std::string> compile(std::string_view)`;
  - `search(line, from)` returns a `std::optional<Match>` with the group spans as byte
    offsets;
  - `matchAt(line, from)`, an anchored match, for replacement;
  - `expand(template, line, match)` builds the replacement text.
- **Parser:** recursive descent over the pattern, building an AST. Recursion depth is
  bounded by a cap on group nesting, say 100.
- **Compiler:** turns the AST into Pike VM instructions:
  - `Char`, `Class` (sorted code-point ranges), `Any`;
  - `Split` (with priority), `Jmp`, `Save`;
  - `AssertBol`, `AssertEol`, `AssertWordB`, `AssertNotWordB`;
  - `Match`.
- **Text adapters:** the VM runs over one line (`search`, `matchAt`) or over a
  `LineSource` (`searchLines`, `matchAtLines`), which presents lines as one text with a
  line break between each two; positions are byte offsets into it, turned back into
  points at the end. Only patterns that can match a line break take the second path.
- **Pike VM:** one pass over the line, one code point per step, with thread lists in
  priority order and per-thread capture slots. An unanchored search adds a fresh thread at
  each position until the first match is found.
- **Literal shortcuts:** the literal text every match starts with (through groups and
  zero-width assertions) lets the search jump to its occurrences with `find()`, and a
  longer literal every match contains rejects a line that lacks it. The remaining cases
  run at roughly 50–80 MB/s.
- **`regex_search.hpp`:** `findFirstRegex`, `findLastRegex`, `findNextRegexInBuffer`,
  `substituteCaptures` and `replaceAllRegexForward` take a `Regex` instead of a
  `std::regex`. Their behaviour (context before the start, empty-match stepping, one history
  version per replace-all) stays the same.
- **Tests:**
  - unit tests per syntax element and error message;
  - the current regex tests, adjusted where this spec differs;
  - linear-time tests: the patterns `parse_fuzz` reports as `HANG` today must finish in
    milliseconds on 200 KB lines.
- **Fuzzing:** a differential harness against `std::regex` for patterns inside the shared
  subset, on ASCII lines where bytes and code points agree, excluding captures inside
  repetitions. Also random patterns must either compile or be refused, and never crash
  or run slower than linear.
- **Docs:**
  - The README gets a section on the dialect.
  - The in-line help (Esc-?) gets the section below.
  - `docs/architecture.md` gets the new class.
  - The "Known limitation" paragraph goes.

## In-line help section

At the end of the left column of `HelpBuffer` (`src/buffer/help_buffer.cpp`), after
"Prompts:", in the same "key (16 columns) + description" layout as the rest. Built with
`formatRawLine()`, since none of it is a shortcut. Keys are at most 13 characters, so a
space always separates them from the description:

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

"As many as fit" is greedy: as many repetitions as still let the rest of the pattern match.
The README carries the `<.*>` / `<.*?>` example.

## Decisions

1. **Lazy quantifiers:** included. They cost almost nothing in a Pike VM: the two branches
   of a `Split` swap priority.
2. **Case-insensitive matching:** not now. A leading `(?i)` with ASCII case folding could be
   added later.
3. **Unicode classes:** no. `\w`, `\d`, `\s` and `\b` stay ASCII, as in ECMAScript without
   the `u` flag.
4. **Group references in replacements:** single digit only. `$10` is group 1 followed by
   `0`.
