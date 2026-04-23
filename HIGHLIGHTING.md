# Syntax Highlighting Reference

## Token Types

| Token          | Description                                        | Modes       |
|----------------|----------------------------------------------------|-------------|
| Normal         | Default text, digits, whitespace                   | All         |
| Keyword        | Language keywords (`if`, `for`, `class`, ...)      | C/C++, JSON |
| Comment        | Line (`//`) and block (`/* */`) comments           | C/C++       |
| String         | `"..."`, `'...'`, raw string literals              | C/C++, JSON |
| Operator       | `+`, `-`, `=`, `&&`, `<<`, `<=>` etc.             | C/C++       |
| Preprocessor   | `#include`, `#define`, and backslash continuations | C/C++       |
| Paren          | `(` `)`                                            | C/C++       |
| Bracket        | `[` `]`                                            | C/C++, JSON |
| Brace          | `{` `}`                                            | C/C++, JSON |
| Angle          | `<` `>` when used as template brackets             | C/C++       |
| Heading        | `# ...` headings                                   | Markdown    |
| Bold           | `**...**` bold text                                | Markdown    |
| Link           | `[text](url)` links                                | Markdown    |
| Number         | Numeric literals                                   | JSON        |

## Colors (24-bit RGB)

| Token         | RGB             | Color Name      | ANSI Escape                |
|---------------|-----------------|-----------------|----------------------------|
| Keyword       | (80, 130, 200)  | Blue            | `\x1b[38;2;80;130;200m`   |
| Comment       | (80, 160, 80)   | Green           | `\x1b[38;2;80;160;80m`    |
| String        | (200, 80, 80)   | Red             | `\x1b[38;2;200;80;80m`    |
| Operator      | (150, 100, 200) | Purple          | `\x1b[38;2;150;100;200m`  |
| Number        | (100, 200, 200) | Light turquoise | `\x1b[38;2;100;200;200m`  |
| Preprocessor  | (180, 140, 60)  | Amber           | `\x1b[38;2;180;140;60m`   |
| Heading       | (80, 130, 200)  | Bold + Blue     | `\x1b[1;38;2;80;130;200m` |
| Bold          | —               | Bold (no color) | `\x1b[1m`                 |
| Link          | (100, 180, 180) | Teal            | `\x1b[38;2;100;180;180m`  |
| Bracket match | (60, 170, 170)  | Turquoise       | `\x1b[38;2;60;170;170m`   |

Paren, Bracket, Brace, and Angle tokens use the default foreground color.

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

- **inBlockComment** — inside `/* ... */`
- **inPreprocessor** — prior line was a `#`-directive ending with `\`
- **rawDelim** — inside `R"delim(...)delim"`, holds the delimiter string
