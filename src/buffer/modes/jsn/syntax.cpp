#include "syntax.hpp"

#include <string_view>

/**
 * @brief True if @p word is a JSON value literal (true/false/null).
 *
 * These share the numeric-literal colour (SyntaxToken::Number).
 */
static bool isJsonValueLiteral(std::string_view word) {
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

    // Literal values: true, false, null.
    if (line[i] >= 'a' && line[i] <= 'z') {
      std::size_t start = i;
      while (i < sz && line[i] >= 'a' && line[i] <= 'z')
        ++i;
      std::string_view word(line.data() + start, i - start);
      if (isJsonValueLiteral(word)) {
        for (std::size_t j = start; j < i; ++j)
          tokens[j] = SyntaxToken::Number;
      }
      continue;
    }

    // Numbers: optional '-', digits, optional '.digits', optional 'e/E[+/-]digits'.
    // RFC 8259 requires at least one digit after '.' and after 'e'/'E', so
    // back off (don't extend the token across a '.' or 'e' that lacks
    // following digits) to keep highlighting honest.
    if ((line[i] >= '0' && line[i] <= '9') ||
        (line[i] == '-' && i + 1 < sz && line[i + 1] >= '0' && line[i + 1] <= '9')) {
      std::size_t start = i;
      if (line[i] == '-')
        ++i;
      while (i < sz && line[i] >= '0' && line[i] <= '9')
        ++i;
      if (i + 1 < sz && line[i] == '.' && line[i + 1] >= '0' && line[i + 1] <= '9') {
        ++i; // '.'
        while (i < sz && line[i] >= '0' && line[i] <= '9')
          ++i;
      }
      if (i < sz && (line[i] == 'e' || line[i] == 'E')) {
        // Exponent must end with at least one digit; require it before
        // committing to extend the number token.
        std::size_t es = i + 1;
        if (es < sz && (line[es] == '+' || line[es] == '-'))
          ++es;
        if (es < sz && line[es] >= '0' && line[es] <= '9') {
          i = es;
          while (i < sz && line[i] >= '0' && line[i] <= '9')
            ++i;
        }
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

/** @brief Return the bracket that pairs with @p c, or 0 if not a JSON bracket. */
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

/** @brief True if @p c is an opening JSON bracket. */
static bool isJsonOpenBracket(char c) { return c == '[' || c == '{'; }

/** @brief Whether @p c with token @p tok is a matchable bracket (not in a string). */
static bool isJsonBracketChar(char c, SyntaxToken tok) {
  if (tok == SyntaxToken::String)
    return false;
  return c == '[' || c == ']' || c == '{' || c == '}';
}

std::optional<BufferPoint> findMatchingBracketJson(std::vector<LinePtr> const& lines,
                                                   BufferPoint pos, LineTokensFn const& getTokens) {
  if (pos.line >= lines.size())
    return std::nullopt;
  auto const& line = *lines[pos.line];
  if (pos.offset >= line.size())
    return std::nullopt;

  auto curTokens = getTokens(pos.line);
  char ch = line[pos.offset];
  auto curTok = (pos.offset < curTokens.size()) ? curTokens[pos.offset] : SyntaxToken::Normal;

  if (!isJsonBracketChar(ch, curTok))
    return std::nullopt;

  char match = jsonMatchingBracket(ch);
  if (!match)
    return std::nullopt;

  if (isJsonOpenBracket(ch)) {
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
    for (std::size_t l = pos.line + 1; l < lines.size(); ++l) {
      auto const& ln = *lines[l];
      auto lnToks = getTokens(l);
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
    for (std::size_t l = pos.line; l > 0;) {
      --l;
      auto const& ln = *lines[l];
      auto lnToks = getTokens(l);
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

std::optional<BufferPoint> findMatchingBracketJson(std::vector<LinePtr> const& lines,
                                                   BufferPoint pos) {
  // Convenience overload that re-tokenizes each visited line from
  // scratch. Intended for tests/ad-hoc use; production callers should
  // thread `cachedLineTokens()` through the callback overload.
  std::vector<std::vector<SyntaxToken>> cache(lines.size());
  std::vector<bool> valid(lines.size(), false);
  auto getTokens = [&](std::size_t i) -> std::span<SyntaxToken const> {
    if (!valid[i]) {
      SyntaxState s;
      tokenizeJsonLine(*lines[i], s, cache[i]);
      valid[i] = true;
    }
    return cache[i];
  };
  return findMatchingBracketJson(lines, pos, getTokens);
}
