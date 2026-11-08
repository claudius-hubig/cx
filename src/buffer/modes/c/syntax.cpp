#include "syntax.hpp"

#include <algorithm>
#include <string_view>

/**
 * @brief Sorted keyword list for binary search.
 *
 * `true`/`false`/`nullptr` are deliberately absent: they are literal
 * values, not keywords, so they are coloured with the numeric-literal
 * colour (see isValueLiteral), consistently with JSON/YAML/Python.
 */
static constexpr std::string_view kKeywords[] = {
    "alignas",
    "alignof",
    "auto",
    "break",
    "case",
    "catch",
    "class",
    "co_await",
    "co_return",
    "co_yield",
    "const",
    "const_cast",
    "consteval",
    "constexpr",
    "constinit",
    "continue",
    "decltype",
    "default",
    "delete",
    "do",
    "dynamic_cast",
    "else",
    "enum",
    "explicit",
    "export",
    "extern",
    "for",
    "friend",
    "goto",
    "if",
    "inline",
    "mutable",
    "namespace",
    "new",
    "noexcept",
    "operator",
    "override",
    "private",
    "protected",
    "public",
    "reinterpret_cast",
    "requires",
    "return",
    "sizeof",
    "static",
    "static_assert",
    "static_cast",
    "struct",
    "switch",
    "template",
    "this",
    "throw",
    "try",
    "typedef",
    "typeid",
    "typename",
    "union",
    "using",
    "virtual",
    "volatile",
    "while",
};
static_assert(std::ranges::is_sorted(kKeywords), "kKeywords must stay sorted for binary_search");

/** @brief True if @p c may start an identifier (letter or underscore). */
static bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/** @brief True if @p c is a decimal digit. */
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

/** @brief True if @p c is a hexadecimal digit. */
static bool isHexDigit(char c) {
  return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/**
 * @brief True if @p c may continue an identifier.
 *
 * Any high-bit byte (>= 0x80) counts so a UTF-8 lead byte does not split an
 * identifier and cause an adjacent ASCII keyword to be highlighted in
 * error. C++ permits non-ASCII identifier characters (UAX #31); this is a
 * coarse approximation that accepts some bytes that are not technically
 * XID_Continue.
 */
static bool isIdentChar(char c) {
  unsigned char u = static_cast<unsigned char>(c);
  return isIdentStart(c) || (u >= '0' && u <= '9') || u >= 0x80;
}

/** @brief True if @p word is a C/C++ keyword. */
static bool isKeyword(std::string_view word) {
  return std::binary_search(std::begin(kKeywords), std::end(kKeywords), word);
}

/**
 * @brief True if @p word is a boolean/null literal.
 *
 * These are literal values, not keywords, so they share the numeric-literal
 * colour (SyntaxToken::Number).
 */
static bool isValueLiteral(std::string_view word) {
  return word == "true" || word == "false" || word == "nullptr";
}

/** @brief True if @p c is a single-character C/C++ operator byte. */
static bool isOperatorChar(char c) {
  switch (c) {
  case '+':
  case '-':
  case '*':
  case '/':
  case '%':
  case '=':
  case '!':
  case '&':
  case '|':
  case '^':
  case '~':
  case '?':
  case ':':
    return true;
  default:
    return false;
  }
}

/**
 * @brief Heuristic: whether the `<`/`>` at @p i is an operator rather than
 *        a template bracket.
 *
 * Operator when surrounded by spaces, or part of `<<`, `>>`, `<=`, `>=`,
 * `<=>`, `->`, `<<=`, or `>>=`.
 */
static bool isAngleOperator(std::string const& line, std::size_t i) {
  char c = line[i];
  std::size_t sz = line.size();
  if (c == '<') {
    // << or <<= → operator
    if (i + 1 < sz && line[i + 1] == '<')
      return true;
    // <= or <=> → operator
    if (i + 1 < sz && line[i + 1] == '=')
      return true;
    // space before AND space after → comparison operator
    if (i > 0 && line[i - 1] == ' ' && i + 1 < sz && line[i + 1] == ' ')
      return true;
    return false;
  }
  if (c == '>') {
    // >> or >>= → operator
    if (i + 1 < sz && line[i + 1] == '>')
      return true;
    // >= → operator
    if (i + 1 < sz && line[i + 1] == '=')
      return true;
    // -> → operator (check for preceding -)
    if (i > 0 && line[i - 1] == '-')
      return true;
    // space before AND space after → comparison operator
    if (i > 0 && line[i - 1] == ' ' && i + 1 < sz && line[i + 1] == ' ')
      return true;
    return false;
  }
  return false;
}

/** @brief True if @p line begins a preprocessor directive (# after optional whitespace). */
static bool startsWithHash(std::string const& line) {
  for (std::size_t j = 0; j < line.size(); ++j) {
    if (line[j] == '#')
      return true;
    if (line[j] != ' ' && line[j] != '\t')
      return false;
  }
  return false;
}

/** @brief True if @p line ends with a backslash (a directive continuation). */
static bool endsWithBackslash(std::string const& line) {
  return !line.empty() && line.back() == '\\';
}

void tokenizeLine(std::string const& line, SyntaxState& state, std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();

  // Preprocessor directive: entire line highlighted when starting with #
  // or continuing from a prior line that ended with backslash. Detect
  // this before the bulk Normal-fill below so we don't pay for the
  // assign twice on preprocessor lines.
  if (!state.inBlockComment && !state.rawDelim.has_value()) {
    if (state.inPreprocessor || startsWithHash(line)) {
      tokens.assign(sz, SyntaxToken::Preprocessor);
      bool continuation = endsWithBackslash(line);
      advanceBlockCommentState(line, state);
      state.inPreprocessor = continuation;
      return;
    }
  }
  state.inPreprocessor = false;
  tokens.assign(sz, SyntaxToken::Normal);
  std::size_t i = 0;

  while (i < sz) {
    // Inside a raw string literal — scan for closing )delim".
    if (state.rawDelim.has_value()) {
      // We need to find )delim" to end the raw string.
      tokens[i] = SyntaxToken::String;
      if (line[i] == ')') {
        auto& d = *state.rawDelim;
        std::size_t dlen = d.size();
        // Check if )delim" follows.
        if (i + dlen + 1 < sz && line[i + dlen + 1] == '"') {
          bool match = true;
          for (std::size_t k = 0; k < dlen; ++k) {
            if (line[i + 1 + k] != d[k]) {
              match = false;
              break;
            }
          }
          if (match) {
            // Mark )delim" as String.
            for (std::size_t k = i; k <= i + dlen + 1; ++k)
              tokens[k] = SyntaxToken::String;
            i += dlen + 2;
            state.rawDelim.reset();
            continue;
          }
        }
      }
      ++i;
      continue;
    }

    // Inside a block comment — scan for */.
    if (state.inBlockComment) {
      tokens[i] = SyntaxToken::Comment;
      if (i + 1 < sz && line[i] == '*' && line[i + 1] == '/') {
        tokens[i + 1] = SyntaxToken::Comment;
        i += 2;
        state.inBlockComment = false;
      } else {
        ++i;
      }
      continue;
    }

    // Line comment — rest of line is comment.
    if (i + 1 < sz && line[i] == '/' && line[i + 1] == '/') {
      for (std::size_t j = i; j < sz; ++j)
        tokens[j] = SyntaxToken::Comment;
      return;
    }

    // Block comment start.
    if (i + 1 < sz && line[i] == '/' && line[i + 1] == '*') {
      tokens[i] = SyntaxToken::Comment;
      tokens[i + 1] = SyntaxToken::Comment;
      i += 2;
      state.inBlockComment = true;
      continue;
    }

    // Raw string literal: R"delim(...
    // Check for R" pattern. The preceding character must not be identifier char
    // (to avoid matching e.g. MYMACRR"...").
    if (line[i] == 'R' && i + 1 < sz && line[i + 1] == '"' &&
        (i == 0 || !isIdentChar(line[i - 1]))) {
      std::size_t dstart = i + 2;
      // Scan for opening ( to find the delimiter.
      std::size_t paren = dstart;
      while (paren < sz && line[paren] != '(')
        ++paren;
      if (paren < sz) {
        // Use a string_view for the in-line scan; only materialize a
        // std::string when the literal crosses a line boundary and we
        // need to persist the delimiter into state.
        std::string_view delim(line.data() + dstart, paren - dstart);
        // Mark R"delim( as String.
        for (std::size_t k = i; k <= paren; ++k)
          tokens[k] = SyntaxToken::String;
        i = paren + 1;
        // Now scan for closing )delim" on same line.
        bool closed = false;
        while (i < sz) {
          tokens[i] = SyntaxToken::String;
          if (line[i] == ')') {
            std::size_t dlen = delim.size();
            if (i + dlen + 1 < sz && line[i + dlen + 1] == '"') {
              bool match = true;
              for (std::size_t k = 0; k < dlen; ++k) {
                if (line[i + 1 + k] != delim[k]) {
                  match = false;
                  break;
                }
              }
              if (match) {
                for (std::size_t k = i; k <= i + dlen + 1; ++k)
                  tokens[k] = SyntaxToken::String;
                i += dlen + 2;
                closed = true;
                break;
              }
            }
          }
          ++i;
        }
        if (!closed)
          state.rawDelim = std::string(delim);
        continue;
      }
    }

    // Regular string literal "...".
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

    // Character literal '...' — but NOT digit separator (e.g. 1'000).
    // The "preceded-by-digit" check is a coarse heuristic and will
    // misclassify contrived constructs like `0'a'` (two adjacent tokens
    // that are not legal C++ in any common context); we accept that
    // limitation in exchange for catching the common 1'000 case.
    if (line[i] == '\'' && !(i > 0 && line[i - 1] >= '0' && line[i - 1] <= '9')) {
      tokens[i] = SyntaxToken::String;
      ++i;
      while (i < sz && line[i] != '\'') {
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

    // Numeric literal. Starts with a digit, or a '.' immediately
    // followed by a digit (e.g. .5f). A leading sign is left to the
    // operator branch, matching the Python/JSON tokenizers. Digit
    // separators (1'000), 0x/0b prefixes, fraction, exponent (e/E for
    // decimal, p/P for hex floats) and the trailing suffix run
    // (u/l/f/z combinations, user-defined-literal suffixes) are all
    // swept into one Number token. The grammar check is deliberately
    // coarse — anything shaped like a number is coloured as one.
    if (isDigit(line[i]) || (line[i] == '.' && i + 1 < sz && isDigit(line[i + 1]))) {
      std::size_t start = i;
      if (line[i] == '0' && i + 1 < sz &&
          (line[i + 1] == 'x' || line[i + 1] == 'X' || line[i + 1] == 'b' || line[i + 1] == 'B')) {
        bool hex = (line[i + 1] == 'x' || line[i + 1] == 'X');
        i += 2;
        while (i < sz && (line[i] == '\'' ||
                          (hex ? isHexDigit(line[i]) : (line[i] == '0' || line[i] == '1'))))
          ++i;
        // Hex float: optional fraction and binary exponent (0x1.8p3).
        if (hex && i < sz && line[i] == '.') {
          ++i;
          while (i < sz && (line[i] == '\'' || isHexDigit(line[i])))
            ++i;
        }
        if (hex && i < sz && (line[i] == 'p' || line[i] == 'P')) {
          ++i;
          if (i < sz && (line[i] == '+' || line[i] == '-'))
            ++i;
          while (i < sz && (line[i] == '\'' || isDigit(line[i])))
            ++i;
        }
      } else {
        while (i < sz && (line[i] == '\'' || isDigit(line[i])))
          ++i;
        if (i < sz && line[i] == '.') {
          ++i;
          while (i < sz && (line[i] == '\'' || isDigit(line[i])))
            ++i;
        }
        if (i < sz && (line[i] == 'e' || line[i] == 'E')) {
          ++i;
          if (i < sz && (line[i] == '+' || line[i] == '-'))
            ++i;
          while (i < sz && (line[i] == '\'' || isDigit(line[i])))
            ++i;
        }
      }
      // Suffix run: u/U/l/L/f/F/z/Z and combinations, plus any
      // user-defined-literal suffix (_id). Identifier-class bytes only.
      while (i < sz && isIdentChar(line[i]))
        ++i;
      for (std::size_t j = start; j < i; ++j)
        tokens[j] = SyntaxToken::Number;
      continue;
    }

    // Identifier — check if it's a keyword.
    if (isIdentStart(line[i])) {
      std::size_t start = i;
      while (i < sz && isIdentChar(line[i]))
        ++i;
      std::string_view word(line.data() + start, i - start);
      if (isValueLiteral(word)) {
        for (std::size_t j = start; j < i; ++j)
          tokens[j] = SyntaxToken::Number;
      } else if (isKeyword(word)) {
        for (std::size_t j = start; j < i; ++j)
          tokens[j] = SyntaxToken::Keyword;
      }
      continue;
    }

    // Angle brackets — use heuristic to distinguish template vs operator.
    if (line[i] == '<' || line[i] == '>') {
      if (isAngleOperator(line, i)) {
        tokens[i] = SyntaxToken::Operator;
        ++i;
        // Consume second char of multi-char operators (<<, >>, <=, >=, <=>, <<=, >>=).
        if (i < sz && (line[i] == '<' || line[i] == '>' || line[i] == '=')) {
          tokens[i] = SyntaxToken::Operator;
          ++i;
          // Third char: <<= or >>= or <=>
          if (i < sz && (line[i] == '=' || line[i] == '>')) {
            tokens[i] = SyntaxToken::Operator;
            ++i;
          }
        }
      } else {
        tokens[i] = SyntaxToken::Angle;
        ++i;
      }
      continue;
    }

    // Brackets: () [] {}
    if (line[i] == '(' || line[i] == ')') {
      tokens[i] = SyntaxToken::Paren;
      ++i;
      continue;
    }
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

    // Operators. Cap at 3 characters: real C++ operators are at most 3
    // chars long (<<=, >>=, ->* etc.) so longer runs of operator-class
    // characters are likely two adjacent tokens that should be styled
    // identically without being merged into one logical operator.
    if (isOperatorChar(line[i])) {
      tokens[i] = SyntaxToken::Operator;
      ++i;
      std::size_t merged = 1;
      while (i < sz && merged < 3 && isOperatorChar(line[i])) {
        tokens[i] = SyntaxToken::Operator;
        ++i;
        ++merged;
      }
      // Any further operator-class chars get their own single-char tokens
      // via subsequent loop iterations.
      continue;
    }

    // Everything else is Normal (digits, whitespace, etc.).
    ++i;
  }
}

void advanceBlockCommentState(std::string const& line, SyntaxState& state) {
  // State-only scan: maintains block-comment, raw-string, and
  // preprocessor continuation state while stepping over strings,
  // character literals, line comments, and raw string literals.
  std::size_t i = 0, sz = line.size();

  // Track preprocessor continuation state.
  if (!state.inBlockComment && !state.rawDelim.has_value()) {
    if (state.inPreprocessor || startsWithHash(line))
      state.inPreprocessor = endsWithBackslash(line);
    else
      state.inPreprocessor = false;
  } else {
    state.inPreprocessor = false;
  }
  while (i < sz) {
    // Inside a raw string literal — scan for closing )delim".
    if (state.rawDelim.has_value()) {
      if (line[i] == ')') {
        auto const& d = *state.rawDelim;
        std::size_t dlen = d.size();
        if (i + dlen + 1 < sz && line[i + dlen + 1] == '"') {
          bool match = true;
          for (std::size_t k = 0; k < dlen; ++k) {
            if (line[i + 1 + k] != d[k]) {
              match = false;
              break;
            }
          }
          if (match) {
            i += dlen + 2;
            state.rawDelim.reset();
            continue;
          }
        }
      }
      ++i;
      continue;
    }

    // Inside a block comment — scan for */.
    if (state.inBlockComment) {
      if (i + 1 < sz && line[i] == '*' && line[i + 1] == '/') {
        i += 2;
        state.inBlockComment = false;
      } else {
        ++i;
      }
      continue;
    }

    // Line comment — rest of line is ignored, no state change.
    if (i + 1 < sz && line[i] == '/' && line[i + 1] == '/')
      return;

    // Block comment start.
    if (i + 1 < sz && line[i] == '/' && line[i + 1] == '*') {
      i += 2;
      state.inBlockComment = true;
      continue;
    }

    // Raw string literal: R"delim(...)delim". Only recognized when the
    // R is not preceded by an identifier character (to avoid matching
    // trailing R in a user identifier).
    if (line[i] == 'R' && i + 1 < sz && line[i + 1] == '"' &&
        (i == 0 || !isIdentChar(line[i - 1]))) {
      std::size_t dstart = i + 2;
      std::size_t paren = dstart;
      while (paren < sz && line[paren] != '(')
        ++paren;
      if (paren < sz) {
        std::string_view delim(line.data() + dstart, paren - dstart);
        i = paren + 1;
        bool closed = false;
        while (i < sz) {
          if (line[i] == ')') {
            std::size_t dlen = delim.size();
            if (i + dlen + 1 < sz && line[i + dlen + 1] == '"') {
              bool match = true;
              for (std::size_t k = 0; k < dlen; ++k) {
                if (line[i + 1 + k] != delim[k]) {
                  match = false;
                  break;
                }
              }
              if (match) {
                i += dlen + 2;
                closed = true;
                break;
              }
            }
          }
          ++i;
        }
        if (!closed)
          state.rawDelim = std::string(delim);
        continue;
      }
    }

    // Regular string literal "..." — single line, no state change.
    if (line[i] == '"') {
      ++i;
      while (i < sz && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < sz)
          ++i;
        ++i;
      }
      if (i < sz)
        ++i;
      continue;
    }

    // Character literal '...' — but NOT digit separator (e.g. 1'000).
    // The "preceded-by-digit" check is a coarse heuristic and will
    // misclassify contrived constructs like `0'a'` (two adjacent tokens
    // that are not legal C++ in any common context); we accept that
    // limitation in exchange for catching the common 1'000 case.
    if (line[i] == '\'' && !(i > 0 && line[i - 1] >= '0' && line[i - 1] <= '9')) {
      ++i;
      while (i < sz && line[i] != '\'') {
        if (line[i] == '\\' && i + 1 < sz)
          ++i;
        ++i;
      }
      if (i < sz)
        ++i;
      continue;
    }

    ++i;
  }
}

/** @brief Return the bracket that pairs with @p c, or 0 if @p c is not a bracket. */
static char matchingBracket(char c) {
  switch (c) {
  case '(':
    return ')';
  case ')':
    return '(';
  case '[':
    return ']';
  case ']':
    return '[';
  case '{':
    return '}';
  case '}':
    return '{';
  case '<':
    return '>';
  case '>':
    return '<';
  default:
    return 0;
  }
}

/** @brief True if @p c is an opening bracket. */
static bool isOpenBracket(char c) { return c == '(' || c == '[' || c == '{' || c == '<'; }

/**
 * @brief Whether @p c with token @p tok counts as a bracket during matching.
 *
 * Brackets inside strings, comments, and preprocessor lines are skipped;
 * angle brackets count only when tokenized as Angle (not Operator).
 */
static bool isBracketChar(char c, SyntaxToken tok) {
  if (tok == SyntaxToken::Comment || tok == SyntaxToken::String || tok == SyntaxToken::Preprocessor)
    return false;
  if ((c == '<' || c == '>') && tok != SyntaxToken::Angle)
    return false;
  return true;
}

std::optional<BufferPoint> findMatchingBracket(std::vector<LinePtr> const& lines, BufferPoint pos,
                                               LineTokensFn const& getTokens) {
  if (pos.line >= lines.size())
    return std::nullopt;
  auto const& line = *lines[pos.line];
  if (pos.offset >= line.size())
    return std::nullopt;

  auto curTokens = getTokens(pos.line);
  char ch = line[pos.offset];
  auto curTok = (pos.offset < curTokens.size()) ? curTokens[pos.offset] : SyntaxToken::Normal;

  // Cursor must be on a syntactically meaningful bracket character.
  if (!isBracketChar(ch, curTok))
    return std::nullopt;

  char match = matchingBracket(ch);
  if (!match)
    return std::nullopt;

  if (isOpenBracket(ch)) {
    int depth = 1;
    // Remainder of pos.line.
    for (std::size_t o = pos.offset + 1; o < line.size(); ++o) {
      auto tok = (o < curTokens.size()) ? curTokens[o] : SyntaxToken::Normal;
      if (isBracketChar(line[o], tok)) {
        if (line[o] == ch)
          ++depth;
        else if (line[o] == match && --depth == 0)
          return BufferPoint{pos.line, o};
      }
    }
    // Subsequent lines.
    for (std::size_t l = pos.line + 1; l < lines.size(); ++l) {
      auto const& ln = *lines[l];
      auto lnToks = getTokens(l);
      for (std::size_t o = 0; o < ln.size(); ++o) {
        auto tok = (o < lnToks.size()) ? lnToks[o] : SyntaxToken::Normal;
        if (isBracketChar(ln[o], tok)) {
          if (ln[o] == ch)
            ++depth;
          else if (ln[o] == match && --depth == 0)
            return BufferPoint{l, o};
        }
      }
    }
  } else {
    int depth = 1;
    // pos.line from pos.offset-1 down to 0.
    for (std::size_t o = pos.offset; o > 0;) {
      --o;
      auto tok = (o < curTokens.size()) ? curTokens[o] : SyntaxToken::Normal;
      if (isBracketChar(line[o], tok)) {
        if (line[o] == ch)
          ++depth;
        else if (line[o] == match && --depth == 0)
          return BufferPoint{pos.line, o};
      }
    }
    // Earlier lines.
    for (std::size_t l = pos.line; l > 0;) {
      --l;
      auto const& ln = *lines[l];
      auto lnToks = getTokens(l);
      for (std::size_t o = ln.size(); o > 0;) {
        --o;
        auto tok = (o < lnToks.size()) ? lnToks[o] : SyntaxToken::Normal;
        if (isBracketChar(ln[o], tok)) {
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

std::optional<BufferPoint> findMatchingBracket(std::vector<LinePtr> const& lines, BufferPoint pos) {
  // Convenience overload for callers without a syntax cache (tests,
  // ad-hoc uses). Memoizes tokens across the call so each visited line
  // is tokenized at most once, starting from the file head.
  struct LocalCache {
    std::vector<LinePtr> const& lines;
    std::vector<std::vector<SyntaxToken>> tokens;
    std::size_t validUpto = 0;
    SyntaxState stateAfterValid{};

    explicit LocalCache(std::vector<LinePtr> const& l) : lines(l), tokens(l.size()) {}

    std::span<SyntaxToken const> at(std::size_t i) {
      if (i < validUpto)
        return tokens[i];
      while (validUpto <= i) {
        tokenizeLine(*lines[validUpto], stateAfterValid, tokens[validUpto]);
        ++validUpto;
      }
      return tokens[i];
    }
  };
  LocalCache cache(lines);
  return findMatchingBracket(lines, pos, [&](std::size_t i) { return cache.at(i); });
}
