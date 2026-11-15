#include "syntax.hpp"

#include <string_view>

/** @brief True if @p w is a shell reserved word. */
static bool isShellKeyword(std::string_view w) {
  static constexpr std::string_view kw[] = {
      "if",    "then",   "else",    "elif",     "fi",       "case",     "esac",   "for",
      "while", "until",  "do",      "done",     "in",       "function", "select", "return",
      "local", "export", "declare", "readonly", "continue", "break",    "time"};
  for (auto k : kw)
    if (w == k)
      return true;
  return false;
}

/** @brief True if @p c is an identifier/word character. */
static bool isWordChar(char c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

void tokenizeShellLine(std::string const& line, ShSyntaxState& state,
                       std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  tokens.assign(sz, SyntaxToken::Normal);

  // Inside an open heredoc: the whole line is a String span until a
  // line whose content equals the tag. For the `<<-` form, leading
  // tabs are stripped before the comparison and the terminator may be
  // indented with tabs.
  if (state.heredocTag.has_value()) {
    std::string_view body(line);
    if (state.heredocStripTabs) {
      std::size_t s = 0;
      while (s < body.size() && body[s] == '\t')
        ++s;
      body.remove_prefix(s);
    }
    if (body == *state.heredocTag) {
      // Terminator line: a plain delimiter, not body text.
      state.heredocTag.reset();
      state.heredocStripTabs = false;
    } else {
      for (std::size_t k = 0; k < sz; ++k)
        tokens[k] = SyntaxToken::String;
    }
    return;
  }

  std::size_t i = 0;
  while (i < sz) {
    char c = line[i];

    // Comment: '#' at line start or after whitespace. A '#' that is
    // part of $#/${#...} never reaches here — the '$' arm consumes it
    // first — so this simple rule does not misfire on those.
    if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
      for (; i < sz; ++i)
        tokens[i] = SyntaxToken::Comment;
      break;
    }

    // Single-quoted string: fully literal, no escapes.
    if (c == '\'') {
      tokens[i++] = SyntaxToken::String;
      while (i < sz && line[i] != '\'')
        tokens[i++] = SyntaxToken::String;
      if (i < sz)
        tokens[i++] = SyntaxToken::String;
      continue;
    }

    // Double-quoted string: backslash escapes; $-expansions are left
    // inside the string span for simplicity.
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

    // Backtick command substitution: treated as one String span.
    if (c == '`') {
      tokens[i++] = SyntaxToken::String;
      while (i < sz && line[i] != '`')
        tokens[i++] = SyntaxToken::String;
      if (i < sz)
        tokens[i++] = SyntaxToken::String;
      continue;
    }

    // Here-string `<<<`: a 3-char operator, consumed as one run so the
    // middle `<` cannot be re-read as the start of a `<<` heredoc.
    if (c == '<' && i + 2 < sz && line[i + 1] == '<' && line[i + 2] == '<') {
      tokens[i++] = SyntaxToken::Operator;
      tokens[i++] = SyntaxToken::Operator;
      tokens[i++] = SyntaxToken::Operator;
      continue;
    }

    // Heredoc introducer: `<<` or `<<-`, but not `<<<` (here-string,
    // already handled above).
    if (c == '<' && i + 1 < sz && line[i + 1] == '<') {
      tokens[i] = SyntaxToken::Operator;
      tokens[i + 1] = SyntaxToken::Operator;
      i += 2;
      bool strip = false;
      if (i < sz && line[i] == '-') {
        strip = true;
        tokens[i++] = SyntaxToken::Operator;
      }
      while (i < sz && (line[i] == ' ' || line[i] == '\t'))
        ++i;
      // Delimiter word: optionally '...'/"..." quoted, or backslash
      // escaped; the tag stored in state is always unquoted.
      if (i < sz) {
        char q = 0;
        if (line[i] == '\'' || line[i] == '"') {
          q = line[i];
          tokens[i++] = SyntaxToken::String;
        } else if (line[i] == '\\') {
          ++i;
        }
        std::size_t ds = i;
        if (q) {
          while (i < sz && line[i] != q)
            tokens[i++] = SyntaxToken::String;
          std::string tag(line, ds, i - ds);
          if (i < sz)
            tokens[i++] = SyntaxToken::String; // closing quote
          if (!tag.empty()) {
            state.heredocTag = std::move(tag);
            state.heredocStripTabs = strip;
          }
        } else {
          while (i < sz && isWordChar(line[i]))
            ++i;
          std::string tag(line, ds, i - ds);
          for (std::size_t k = ds; k < i; ++k)
            tokens[k] = SyntaxToken::String; // delimiter pairs with body
          if (!tag.empty()) {
            state.heredocTag = std::move(tag);
            state.heredocStripTabs = strip;
          }
        }
      }
      continue;
    }

    // Parameter / variable expansion: ${...}, $NAME, $1, $?, $$, $@ ...
    if (c == '$' && i + 1 < sz) {
      char n = line[i + 1];
      if (n == '{') {
        std::size_t s = i;
        i += 2;
        int depth = 1;
        while (i < sz && depth > 0) {
          if (line[i] == '{')
            ++depth;
          else if (line[i] == '}')
            --depth;
          ++i;
        }
        for (std::size_t k = s; k < i; ++k)
          tokens[k] = SyntaxToken::Variable;
        continue;
      }
      if (isWordChar(n) || n == '?' || n == '$' || n == '!' || n == '@' || n == '*' || n == '#' ||
          n == '-') {
        std::size_t s = i;
        i += 2; // '$' + first char
        if (isWordChar(n) && !(n >= '0' && n <= '9'))
          while (i < sz && isWordChar(line[i]))
            ++i;
        for (std::size_t k = s; k < i; ++k)
          tokens[k] = SyntaxToken::Variable;
        continue;
      }
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
    if (c == '|' || c == '&' || c == ';' || c == '<' || c == '>' || c == '=') {
      tokens[i++] = SyntaxToken::Operator;
      continue;
    }

    // Bare word: classify as keyword if it matches the reserved set.
    // Words starting with a digit are not keywords (and are left
    // Normal, since shell has no distinct numeric-literal syntax).
    if (isWordChar(c) && !(c >= '0' && c <= '9')) {
      std::size_t s = i;
      while (i < sz && isWordChar(line[i]))
        ++i;
      if (isShellKeyword(std::string_view(line.data() + s, i - s)))
        for (std::size_t k = s; k < i; ++k)
          tokens[k] = SyntaxToken::Keyword;
      continue;
    }

    ++i;
  }
}
