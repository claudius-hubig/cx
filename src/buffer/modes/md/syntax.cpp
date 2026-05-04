#include "syntax.hpp"

void tokenizeMarkdownLine(std::string const& line, SyntaxState& /*state*/,
                          std::vector<SyntaxToken>& tokens) {
  tokens.assign(line.size(), SyntaxToken::Normal);
  std::size_t sz = line.size();

  // Heading: line starts with one or more '#' followed by a space (or end of line).
  if (!line.empty() && line[0] == '#') {
    std::size_t i = 0;
    while (i < sz && line[i] == '#')
      ++i;
    if (i == sz || line[i] == ' ') {
      for (std::size_t j = 0; j < sz; ++j)
        tokens[j] = SyntaxToken::Heading;
      return;
    }
  }

  // Scan for inline bold (**...**) and links ([text](url)).
  for (std::size_t i = 0; i < sz;) {
    // Bold: **...**
    if (i + 1 < sz && line[i] == '*' && line[i + 1] == '*') {
      std::size_t start = i;
      bool closed = false;
      std::size_t scan = i + 2;
      while (scan + 1 < sz) {
        if (line[scan] == '*' && line[scan + 1] == '*') {
          scan += 2;
          for (std::size_t j = start; j < scan; ++j)
            tokens[j] = SyntaxToken::Bold;
          i = scan;
          closed = true;
          break;
        }
        ++scan;
      }
      if (!closed) {
        // Unclosed: leave the `**` as Normal and continue scanning past it
        // so any subsequent links/markup on the same line still highlight.
        i = start + 2;
      }
      continue;
    }

    // Link: [text](url). Disallow nested '[' inside the label so a
    // mismatched outer `[` does not greedily consume a real link further
    // along the line; on a no-match, resume just past the failed `[`.
    if (line[i] == '[') {
      std::size_t start = i;
      std::size_t scan = i + 1;
      while (scan < sz && line[scan] != ']' && line[scan] != '[')
        ++scan;
      if (scan < sz && line[scan] == ']' && scan + 1 < sz && line[scan + 1] == '(') {
        scan += 2;
        while (scan < sz && line[scan] != ')')
          ++scan;
        if (scan < sz) {
          ++scan;
          for (std::size_t j = start; j < scan; ++j)
            tokens[j] = SyntaxToken::Link;
          i = scan;
          continue;
        }
      }
      // Not a valid link.
      i = start + 1;
      continue;
    }

    ++i;
  }
}
