#include "syntax.hpp"

#include <cctype>
#include <string_view>

static int leadingSpaces(std::string const& l) {
  int n = 0;
  while (n < static_cast<int>(l.size()) && l[n] == ' ')
    ++n;
  return n;
}

static bool isBlank(std::string const& l) {
  for (char c : l)
    if (c != ' ' && c != '\t')
      return false;
  return true;
}

static bool ciEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i])
      return false;
  return true;
}

// YAML 1.1 plain boolean / null scalars (case-insensitive), plus `~`.
static bool isYamlConst(std::string_view w) {
  if (w == "~")
    return true;
  static constexpr std::string_view kw[] = {"true", "false", "yes",  "no",
                                            "on",   "off",   "null", "nan"};
  for (auto k : kw)
    if (ciEqual(w, k))
      return true;
  return false;
}

static bool isYamlNumber(std::string_view w) {
  if (w.empty())
    return false;
  std::size_t i = 0;
  if (w[i] == '+' || w[i] == '-')
    ++i;
  bool digits = false;
  while (i < w.size() && w[i] >= '0' && w[i] <= '9') {
    ++i;
    digits = true;
  }
  if (i < w.size() && w[i] == '.') {
    ++i;
    while (i < w.size() && w[i] >= '0' && w[i] <= '9') {
      ++i;
      digits = true;
    }
  }
  if (!digits)
    return false;
  if (i < w.size() && (w[i] == 'e' || w[i] == 'E')) {
    ++i;
    if (i < w.size() && (w[i] == '+' || w[i] == '-'))
      ++i;
    bool ed = false;
    while (i < w.size() && w[i] >= '0' && w[i] <= '9') {
      ++i;
      ed = true;
    }
    if (!ed)
      return false;
  }
  return i == w.size();
}

void tokenizeYamlLine(std::string const& line, SyntaxState& state,
                      std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  tokens.assign(sz, SyntaxToken::Normal);

  // Inside an open `|`/`>` block scalar: blank lines and lines
  // indented strictly deeper than the introducing column are literal
  // String content. The first non-blank line that dedents back to
  // <= the column ends the block and is parsed normally below.
  if (state.yamlBlockCol >= 0) {
    if (isBlank(line)) {
      for (std::size_t k = 0; k < sz; ++k)
        tokens[k] = SyntaxToken::String;
      return;
    }
    if (leadingSpaces(line) > state.yamlBlockCol) {
      for (std::size_t k = 0; k < sz; ++k)
        tokens[k] = SyntaxToken::String;
      return;
    }
    state.yamlBlockCol = -1;
  }

  int indent = leadingSpaces(line);
  std::size_t i = static_cast<std::size_t>(indent);

  // Directive (%YAML, %TAG): whole line.
  if (i < sz && line[i] == '%') {
    for (std::size_t k = i; k < sz; ++k)
      tokens[k] = SyntaxToken::Preprocessor;
    return;
  }

  // Document markers --- and ... (alone or followed by a space).
  if (sz - i >= 3 && (line.compare(i, 3, "---") == 0 || line.compare(i, 3, "...") == 0) &&
      (i + 3 == sz || line[i + 3] == ' ')) {
    tokens[i] = tokens[i + 1] = tokens[i + 2] = SyntaxToken::Preprocessor;
    i += 3;
  }

  // Leading block-sequence bullets: `- ` possibly repeated.
  while (i < sz && line[i] == '-' && (i + 1 == sz || line[i + 1] == ' ')) {
    tokens[i] = SyntaxToken::Operator;
    ++i;
    while (i < sz && line[i] == ' ')
      ++i;
  }

  // Mapping key: `<key>:` where the colon is followed by a space or
  // ends the line. The key may be a quoted scalar.
  if (i < sz) {
    if (line[i] == '"' || line[i] == '\'') {
      char q = line[i];
      std::size_t e = i + 1;
      while (e < sz && line[e] != q) {
        if (q == '"' && line[e] == '\\' && e + 1 < sz)
          ++e;
        ++e;
      }
      if (e < sz)
        ++e; // past closing quote
      if (e < sz && line[e] == ':' && (e + 1 == sz || line[e + 1] == ' ')) {
        for (std::size_t p = i; p < e; ++p)
          tokens[p] = SyntaxToken::String;
        tokens[e] = SyntaxToken::Operator;
        i = e + 1;
      }
    } else {
      std::size_t e = i;
      while (e < sz && line[e] != ':' && line[e] != '#')
        ++e;
      if (e < sz && line[e] == ':' && (e + 1 == sz || line[e + 1] == ' ')) {
        std::size_t ke = e;
        while (ke > i && line[ke - 1] == ' ')
          --ke;
        for (std::size_t p = i; p < ke; ++p)
          tokens[p] = SyntaxToken::Keyword;
        tokens[e] = SyntaxToken::Operator;
        i = e + 1;
      }
    }
  }

  // Value / inline content.
  while (i < sz) {
    char c = line[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
      for (; i < sz; ++i)
        tokens[i] = SyntaxToken::Comment;
      break;
    }
    if (c == '\'') {
      tokens[i++] = SyntaxToken::String;
      while (i < sz && line[i] != '\'')
        tokens[i++] = SyntaxToken::String;
      if (i < sz)
        tokens[i++] = SyntaxToken::String;
      continue;
    }
    if (c == '"') {
      tokens[i++] = SyntaxToken::String;
      while (i < sz && line[i] != '"') {
        tokens[i] = SyntaxToken::String;
        if (line[i] == '\\' && i + 1 < sz) {
          ++i;
          tokens[i] = SyntaxToken::String;
        }
        ++i;
      }
      if (i < sz)
        tokens[i++] = SyntaxToken::String;
      continue;
    }
    // Anchor (&a), alias (*a), tag (!t / !!type) — node properties.
    if (c == '&' || c == '*' || c == '!') {
      std::size_t s = i++;
      while (i < sz && line[i] != ' ' && line[i] != ',' && line[i] != ']' && line[i] != '}')
        ++i;
      for (std::size_t p = s; p < i; ++p)
        tokens[p] = SyntaxToken::Preprocessor;
      continue;
    }
    // Block scalar indicator `|` / `>` (with optional chomping/indent
    // indicators): valid only when nothing but an optional comment
    // follows on the line.
    if (c == '|' || c == '>') {
      std::size_t j = i + 1;
      while (j < sz && (line[j] == '+' || line[j] == '-' || (line[j] >= '0' && line[j] <= '9')))
        ++j;
      std::size_t t = j;
      while (t < sz && line[t] == ' ')
        ++t;
      if (t == sz || line[t] == '#') {
        for (std::size_t p = i; p < j; ++p)
          tokens[p] = SyntaxToken::Operator;
        if (t < sz && line[t] == '#')
          for (std::size_t p = t; p < sz; ++p)
            tokens[p] = SyntaxToken::Comment;
        state.yamlBlockCol = indent; // body must be more indented
        return;
      }
      tokens[i++] = SyntaxToken::Operator;
      continue;
    }
    if (c == '[' || c == ']') {
      tokens[i++] = SyntaxToken::Bracket;
      continue;
    }
    if (c == '{' || c == '}') {
      tokens[i++] = SyntaxToken::Brace;
      continue;
    }
    if (c == ',' || c == ':') {
      tokens[i++] = SyntaxToken::Operator;
      continue;
    }
    // Plain scalar word: classify booleans/null/numbers.
    std::size_t s = i;
    while (i < sz && line[i] != ' ' && line[i] != ',' && line[i] != ']' && line[i] != '}' &&
           line[i] != '#')
      ++i;
    std::string_view w(line.data() + s, i - s);
    if (isYamlConst(w))
      for (std::size_t p = s; p < i; ++p)
        tokens[p] = SyntaxToken::Keyword;
    else if (isYamlNumber(w))
      for (std::size_t p = s; p < i; ++p)
        tokens[p] = SyntaxToken::Number;
  }
}
