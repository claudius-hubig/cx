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
      i += 2;
      // Find closing **
      while (i + 1 < sz) {
        if (line[i] == '*' && line[i + 1] == '*') {
          i += 2;
          for (std::size_t j = start; j < i; ++j)
            tokens[j] = SyntaxToken::Bold;
          break;
        }
        ++i;
      }
      if (i >= sz)
        break; // unclosed — leave as Normal
      continue;
    }

    // Link: [text](url)
    if (line[i] == '[') {
      std::size_t start = i;
      ++i;
      // Find closing ]
      while (i < sz && line[i] != ']')
        ++i;
      if (i < sz && i + 1 < sz && line[i + 1] == '(') {
        i += 2;
        // Find closing )
        while (i < sz && line[i] != ')')
          ++i;
        if (i < sz) {
          ++i;
          for (std::size_t j = start; j < i; ++j)
            tokens[j] = SyntaxToken::Link;
          continue;
        }
      }
      // Not a valid link, skip
      i = start + 1;
      continue;
    }

    ++i;
  }
}
