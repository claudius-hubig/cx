#include "syntax.hpp"

#include <string_view>

static bool isJsonKeyword(std::string_view word) {
  return word == "true" || word == "false" || word == "null";
}

void tokenizeJsonLine(std::string const& line, SyntaxState& /*state*/,
                      std::vector<SyntaxToken>& tokens) {
  tokens.assign(line.size(), SyntaxToken::Normal);
  std::size_t i = 0;
  std::size_t sz = line.size();

  while (i < sz) {
    // String literal "..." with backslash escapes.
    if (line[i] == '"') {
      tokens[i] = SyntaxToken::String;
      ++i;
      while (i < sz && line[i] != '"') {
        tokens[i] = SyntaxToken::String;
        if (line[i] == '\\' && i + 1 < sz) {
          ++i;
          tokens[i] = SyntaxToken::String;
        }
        ++i;
      }
      if (i < sz) {
        tokens[i] = SyntaxToken::String;
        ++i;
      }
      continue;
    }

    // Keywords: true, false, null.
    if (line[i] >= 'a' && line[i] <= 'z') {
      std::size_t start = i;
      while (i < sz && line[i] >= 'a' && line[i] <= 'z')
        ++i;
      std::string_view word(line.data() + start, i - start);
      if (isJsonKeyword(word)) {
        for (std::size_t j = start; j < i; ++j)
          tokens[j] = SyntaxToken::Keyword;
      }
      continue;
    }

    // Numbers: optional '-', digits, optional '.digits', optional 'e/E[+/-]digits'.
    if ((line[i] >= '0' && line[i] <= '9') ||
        (line[i] == '-' && i + 1 < sz && line[i + 1] >= '0' && line[i + 1] <= '9')) {
      std::size_t start = i;
      if (line[i] == '-')
        ++i;
      while (i < sz && line[i] >= '0' && line[i] <= '9')
        ++i;
      if (i < sz && line[i] == '.') {
        ++i;
        while (i < sz && line[i] >= '0' && line[i] <= '9')
          ++i;
      }
      if (i < sz && (line[i] == 'e' || line[i] == 'E')) {
        ++i;
        if (i < sz && (line[i] == '+' || line[i] == '-'))
          ++i;
        while (i < sz && line[i] >= '0' && line[i] <= '9')
          ++i;
      }
      for (std::size_t j = start; j < i; ++j)
        tokens[j] = SyntaxToken::Number;
      continue;
    }

    // Brackets.
    if (line[i] == '[' || line[i] == ']') {
      tokens[i] = SyntaxToken::Bracket;
      ++i;
      continue;
    }
    if (line[i] == '{' || line[i] == '}') {
      tokens[i] = SyntaxToken::Brace;
      ++i;
      continue;
    }

    // Everything else: colons, commas, whitespace → Normal.
    ++i;
  }
}

// --- Bracket matching for JSON ---

static char jsonMatchingBracket(char c) {
  switch (c) {
  case '[':
    return ']';
  case ']':
    return '[';
  case '{':
    return '}';
  case '}':
    return '{';
  default:
    return 0;
  }
}

static bool isJsonOpenBracket(char c) { return c == '[' || c == '{'; }

// A bracket is matchable if it is not inside a string.
static bool isJsonBracketChar(char c, SyntaxToken tok) {
  if (tok == SyntaxToken::String)
    return false;
  return c == '[' || c == ']' || c == '{' || c == '}';
}

std::optional<BufferPoint> findMatchingBracketJson(std::vector<std::string> const& lines,
                                                   BufferPoint pos) {
  if (pos.line >= lines.size())
    return std::nullopt;
  auto& line = lines[pos.line];
  if (pos.offset >= line.size())
    return std::nullopt;

  // Tokenize the cursor line (no multi-line state needed for JSON).
  SyntaxState unused;
  std::vector<SyntaxToken> curTokens;
  tokenizeJsonLine(line, unused, curTokens);

  char ch = line[pos.offset];
  auto curTok = (pos.offset < curTokens.size()) ? curTokens[pos.offset] : SyntaxToken::Normal;

  if (!isJsonBracketChar(ch, curTok))
    return std::nullopt;

  char match = jsonMatchingBracket(ch);
  if (!match)
    return std::nullopt;

  if (isJsonOpenBracket(ch)) {
    // Scan forward.
    int depth = 1;

    for (std::size_t o = pos.offset + 1; o < line.size(); ++o) {
      auto tok = (o < curTokens.size()) ? curTokens[o] : SyntaxToken::Normal;
      if (isJsonBracketChar(line[o], tok)) {
        if (line[o] == ch)
          ++depth;
        else if (line[o] == match && --depth == 0)
          return BufferPoint{pos.line, o};
      }
    }

    std::vector<SyntaxToken> lnToks;
    for (std::size_t l = pos.line + 1; l < lines.size(); ++l) {
      auto& ln = lines[l];
      SyntaxState s;
      tokenizeJsonLine(ln, s, lnToks);
      for (std::size_t o = 0; o < ln.size(); ++o) {
        auto tok = (o < lnToks.size()) ? lnToks[o] : SyntaxToken::Normal;
        if (isJsonBracketChar(ln[o], tok)) {
          if (ln[o] == ch)
            ++depth;
          else if (ln[o] == match && --depth == 0)
            return BufferPoint{l, o};
        }
      }
    }
  } else {
    // Scan backward.
    int depth = 1;

    for (std::size_t o = pos.offset; o > 0;) {
      --o;
      auto tok = (o < curTokens.size()) ? curTokens[o] : SyntaxToken::Normal;
      if (isJsonBracketChar(line[o], tok)) {
        if (line[o] == ch)
          ++depth;
        else if (line[o] == match && --depth == 0)
          return BufferPoint{pos.line, o};
      }
    }

    std::vector<SyntaxToken> lnToks;
    for (std::size_t l = pos.line; l > 0;) {
      --l;
      auto& ln = lines[l];
      SyntaxState s;
      tokenizeJsonLine(ln, s, lnToks);
      for (std::size_t o = ln.size(); o > 0;) {
        --o;
        auto tok = (o < lnToks.size()) ? lnToks[o] : SyntaxToken::Normal;
        if (isJsonBracketChar(ln[o], tok)) {
          if (ln[o] == ch)
            ++depth;
          else if (ln[o] == match && --depth == 0)
            return BufferPoint{l, o};
        }
      }
    }
  }

  return std::nullopt;
}
