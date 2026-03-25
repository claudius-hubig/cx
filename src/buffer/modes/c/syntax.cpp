#include "syntax.hpp"

#include <algorithm>
#include <string_view>

// Sorted keyword list for binary search.
// Note: "true" and "false" are excluded — they are values, not keywords.
static constexpr std::string_view kKeywords[] = {
    "alignas",  "alignof",  "auto",         "break",     "case",          "catch",
    "class",    "const",    "const_cast",   "consteval", "constexpr",     "constinit",
    "continue", "co_await", "co_return",    "co_yield",  "decltype",      "default",
    "delete",   "do",       "dynamic_cast", "else",      "enum",          "explicit",
    "export",   "extern",   "for",          "friend",    "goto",          "if",
    "inline",   "mutable",  "namespace",    "new",       "noexcept",      "nullptr",
    "operator", "override", "private",      "protected", "public",        "reinterpret_cast",
    "requires", "return",   "sizeof",       "static",    "static_assert", "static_cast",
    "struct",   "switch",   "template",     "this",      "throw",         "try",
    "typedef",  "typeid",   "typename",     "union",     "using",         "virtual",
    "volatile", "while",
};

static bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

static bool isKeyword(std::string_view word) {
  return std::binary_search(std::begin(kKeywords), std::end(kKeywords), word);
}

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

// Heuristic: < or > used as an operator (not a template bracket).
// Operator if: preceded by a space AND followed by a space, or part of
// <<, >>, <=, >=, <=>, ->, or <<= / >>=.
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

std::vector<SyntaxToken> tokenizeLine(std::string const& line, SyntaxState& state) {
  std::vector<SyntaxToken> tokens(line.size(), SyntaxToken::Normal);
  std::size_t i = 0;
  std::size_t sz = line.size();

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
      return tokens;
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
        std::string delim(line.data() + dstart, paren - dstart);
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
          state.rawDelim = delim;
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

    // Identifier — check if it's a keyword.
    if (isIdentStart(line[i])) {
      std::size_t start = i;
      while (i < sz && isIdentChar(line[i]))
        ++i;
      std::string_view word(line.data() + start, i - start);
      if (isKeyword(word)) {
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

    // Operators.
    if (isOperatorChar(line[i])) {
      tokens[i] = SyntaxToken::Operator;
      ++i;
      // Consume multi-character operators (e.g. !=, &&, ||, >>=, etc.).
      while (i < sz && isOperatorChar(line[i])) {
        tokens[i] = SyntaxToken::Operator;
        ++i;
      }
      continue;
    }

    // Everything else is Normal (digits, whitespace, etc.).
    ++i;
  }

  return tokens;
}

void advanceBlockCommentState(std::string const& line, SyntaxState& state) {
  // tokenizeLine already performs the full scan and updates `state` as a side
  // effect. Discard the returned token vector — only the state matters here.
  tokenizeLine(line, state);
}

// Bracket pairing table.
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

static bool isOpenBracket(char c) { return c == '(' || c == '[' || c == '{' || c == '<'; }

// Returns true if the character `c` with token `tok` should be counted as a
// bracket during matching. Brackets inside strings and comments are skipped.
// Angle brackets are only counted when tokenized as Angle (not Operator).
static bool isBracketChar(char c, SyntaxToken tok) {
  if (tok == SyntaxToken::Comment || tok == SyntaxToken::String)
    return false;
  if ((c == '<' || c == '>') && tok != SyntaxToken::Angle)
    return false;
  return true;
}

std::optional<BufferPoint> findMatchingBracket(std::vector<std::string> const& lines,
                                               BufferPoint pos) {
  if (pos.line >= lines.size())
    return std::nullopt;
  auto& line = lines[pos.line];
  if (pos.offset >= line.size())
    return std::nullopt;

  // Compute syntax state at the start of pos.line by scanning preceding lines.
  SyntaxState stateAtLine;
  for (std::size_t i = 0; i < pos.line; ++i)
    advanceBlockCommentState(lines[i], stateAtLine);

  // Tokenize pos.line; stateAfterLine becomes the state at the start of the next line.
  SyntaxState stateAfterLine = stateAtLine;
  auto curTokens = tokenizeLine(line, stateAfterLine);

  char ch = line[pos.offset];
  auto curTok = (pos.offset < curTokens.size()) ? curTokens[pos.offset] : SyntaxToken::Normal;

  // Cursor must be on a syntactically meaningful bracket character.
  if (!isBracketChar(ch, curTok))
    return std::nullopt;

  char match = matchingBracket(ch);
  if (!match)
    return std::nullopt;

  if (isOpenBracket(ch)) {
    // Scan forward through the remainder of pos.line, then subsequent lines.
    int depth = 1;
    SyntaxState state = stateAfterLine; // state at start of next line

    for (std::size_t o = pos.offset + 1; o < line.size(); ++o) {
      auto tok = (o < curTokens.size()) ? curTokens[o] : SyntaxToken::Normal;
      if (isBracketChar(line[o], tok)) {
        if (line[o] == ch)
          ++depth;
        else if (line[o] == match && --depth == 0)
          return BufferPoint{pos.line, o};
      }
    }

    for (std::size_t l = pos.line + 1; l < lines.size(); ++l) {
      auto& ln = lines[l];
      auto lnToks = tokenizeLine(ln, state); // advances state past ln
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
    // Scan backward. Precompute syntax states at the start of each line so we
    // can tokenize arbitrary lines without rescanning from the beginning each time.
    std::vector<SyntaxState> lineStartStates(pos.line + 1);
    {
      SyntaxState s;
      for (std::size_t i = 0; i <= pos.line; ++i) {
        lineStartStates[i] = s;
        if (i < pos.line)
          advanceBlockCommentState(lines[i], s);
      }
    }

    int depth = 1;

    // Scan pos.line from pos.offset-1 down to 0.
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

    // Scan lines before pos.line, each tokenized from its cached start state.
    for (std::size_t l = pos.line; l > 0;) {
      --l;
      auto& ln = lines[l];
      SyntaxState s = lineStartStates[l];
      auto lnToks = tokenizeLine(ln, s);
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
