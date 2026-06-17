#include "syntax.hpp"

#include <string_view>

/** @brief True if @p c is an identifier/word character. */
static bool isWordChar(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/** @brief True if @p c may start an identifier. */
static bool isWordStart(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/** @brief True if @p c is a decimal digit. */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/** @brief True if @p c is a Python string-literal prefix (r/b/f/u, any case). */
static bool isStringPrefix(char c) {
  return c == 'r' || c == 'b' || c == 'f' || c == 'u' || c == 'R' || c == 'B' || c == 'F' ||
         c == 'U';
}

/** @brief True if @p w is a Python keyword. */
static bool isPyKeyword(std::string_view w) {
  static constexpr std::string_view kw[] = {
      "def",     "class",    "if",     "elif",     "else",   "for",    "while", "try",    "except",
      "finally", "with",     "as",     "import",   "from",   "return", "yield", "lambda", "pass",
      "break",   "continue", "global", "nonlocal", "assert", "del",    "raise", "async",  "await",
      "match",   "case",     "and",    "or",       "not",    "is",     "in"};
  for (auto k : kw)
    if (w == k)
      return true;
  return false;
}

/**
 * @brief True if @p w is a Python value literal (None/True/False).
 *
 * These share the numeric-literal colour (SyntaxToken::Number).
 */
static bool isPyValueLiteral(std::string_view w) {
  return w == "None" || w == "True" || w == "False";
}

/**
 * @brief Scan a triple-quoted body starting at @p i, marking every byte
 *        (and the closing `qqq`) as String.
 *
 * @return True (advancing @p i past the closing quote) if the string closes
 *         on this line; otherwise marks the rest of the line String, leaves
 *         @p i == @p sz, and returns false.
 */
static bool scanTriple(std::string const& line, std::size_t sz, std::size_t& i, char q, bool raw,
                       std::vector<SyntaxToken>& tokens) {
  while (i < sz) {
    if (!raw && line[i] == '\\' && i + 1 < sz) {
      tokens[i] = SyntaxToken::String;
      tokens[i + 1] = SyntaxToken::String;
      i += 2;
      continue;
    }
    if (line[i] == q && i + 2 < sz && line[i + 1] == q && line[i + 2] == q) {
      tokens[i] = tokens[i + 1] = tokens[i + 2] = SyntaxToken::String;
      i += 3;
      return true;
    }
    tokens[i] = SyntaxToken::String;
    ++i;
  }
  return false;
}

void tokenizePythonLine(std::string const& line, SyntaxState& state,
                        std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  tokens.assign(sz, SyntaxToken::Normal);
  std::size_t i = 0;

  // Continuation of a triple-quoted string from a previous line.
  if (state.pyTriple != 0) {
    if (!scanTriple(line, sz, i, state.pyTriple, state.pyTripleRaw, tokens))
      return; // still open
    state.pyTriple = 0;
    state.pyTripleRaw = false;
  }

  // Index of the first non-whitespace byte (for decorator detection).
  std::size_t firstNonWs = 0;
  while (firstNonWs < sz && (line[firstNonWs] == ' ' || line[firstNonWs] == '\t'))
    ++firstNonWs;

  while (i < sz) {
    char c = line[i];

    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }

    // Comment: `#` outside a string runs to end of line.
    if (c == '#') {
      for (; i < sz; ++i)
        tokens[i] = SyntaxToken::Comment;
      break;
    }

    // String, optionally with an r/b/f/u prefix (up to two letters).
    if (c == '\'' || c == '"' || isStringPrefix(c)) {
      std::size_t p = i;
      while (p < sz && isStringPrefix(line[p]) && p - i < 2)
        ++p;
      if (p < sz && (line[p] == '\'' || line[p] == '"')) {
        bool raw = false;
        for (std::size_t k = i; k < p; ++k)
          if (line[k] == 'r' || line[k] == 'R')
            raw = true;
        char q = line[p];
        bool triple = (p + 2 < sz && line[p + 1] == q && line[p + 2] == q);
        std::size_t start = i;
        if (triple) {
          for (std::size_t k = start; k < p + 3; ++k)
            tokens[k] = SyntaxToken::String;
          i = p + 3;
          if (!scanTriple(line, sz, i, q, raw, tokens)) {
            state.pyTriple = q;
            state.pyTripleRaw = raw;
            return;
          }
          continue;
        }
        for (std::size_t k = start; k <= p; ++k)
          tokens[k] = SyntaxToken::String;
        i = p + 1;
        while (i < sz && line[i] != q) {
          tokens[i] = SyntaxToken::String;
          if (!raw && line[i] == '\\' && i + 1 < sz) {
            ++i;
            tokens[i] = SyntaxToken::String;
          }
          ++i;
        }
        if (i < sz)
          tokens[i++] = SyntaxToken::String;
        continue;
      }
      // Prefix letters not followed by a quote: ordinary identifier.
    }

    // Decorator: `@name` (dotted) when it is the line's first token.
    if (c == '@' && i == firstNonWs) {
      std::size_t s = i++;
      while (i < sz && (isWordChar(line[i]) || line[i] == '.'))
        ++i;
      for (std::size_t k = s; k < i; ++k)
        tokens[k] = SyntaxToken::Preprocessor;
      continue;
    }

    // Numbers: 0x/0o/0b prefixes, digit groups with `_`, fraction,
    // exponent, optional imaginary `j`.
    if (isDigit(c) || (c == '.' && i + 1 < sz && isDigit(line[i + 1]))) {
      std::size_t s = i;
      if (c == '0' && i + 1 < sz &&
          (line[i + 1] == 'x' || line[i + 1] == 'X' || line[i + 1] == 'o' || line[i + 1] == 'O' ||
           line[i + 1] == 'b' || line[i + 1] == 'B')) {
        i += 2;
        while (i < sz && (isWordChar(line[i]) || line[i] == '_'))
          ++i;
      } else {
        while (i < sz && (isDigit(line[i]) || line[i] == '_'))
          ++i;
        if (i < sz && line[i] == '.') {
          ++i;
          while (i < sz && (isDigit(line[i]) || line[i] == '_'))
            ++i;
        }
        if (i < sz && (line[i] == 'e' || line[i] == 'E')) {
          ++i;
          if (i < sz && (line[i] == '+' || line[i] == '-'))
            ++i;
          while (i < sz && (isDigit(line[i]) || line[i] == '_'))
            ++i;
        }
        if (i < sz && (line[i] == 'j' || line[i] == 'J'))
          ++i;
      }
      for (std::size_t k = s; k < i; ++k)
        tokens[k] = SyntaxToken::Number;
      continue;
    }

    // Identifier / keyword / literal value.
    if (isWordStart(c)) {
      std::size_t s = i;
      while (i < sz && isWordChar(line[i]))
        ++i;
      std::string_view w(line.data() + s, i - s);
      if (isPyValueLiteral(w))
        for (std::size_t k = s; k < i; ++k)
          tokens[k] = SyntaxToken::Number;
      else if (isPyKeyword(w))
        for (std::size_t k = s; k < i; ++k)
          tokens[k] = SyntaxToken::Keyword;
      continue;
    }

    if (c == '(' || c == ')') {
      tokens[i++] = SyntaxToken::Paren;
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
    if (c == ':') {
      tokens[i++] = SyntaxToken::Operator;
      continue;
    }

    ++i;
  }
}
