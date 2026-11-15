#include "syntax.hpp"

#include <algorithm>
#include <optional>
#include <string_view>

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static bool isBareKeyChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) || c == '_' || c == '-';
}

/**
 * @brief End of the single-line string whose opening quote is at @p i:
 *        the index past the closing quote, or @c std::nullopt if the line
 *        ends first. Backslash escapes apply to `"` strings only.
 */
static std::optional<std::size_t> stringEnd(std::string const& line, std::size_t i) {
  char q = line[i];
  for (std::size_t e = i + 1; e < line.size(); ++e) {
    if (q == '"' && line[e] == '\\')
      ++e;
    else if (line[e] == q)
      return e + 1;
  }
  return std::nullopt;
}

/**
 * @brief Mark multi-line string content from @p i as String up to and
 *        including the closing triple @p q. Advances @p i past it and
 *        returns true, or returns false with @p i at the line end.
 */
static bool scanMultiBody(std::string const& line, std::size_t& i, char q,
                          std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  std::string_view triple = q == '"' ? R"(""")" : "'''";
  while (i < sz) {
    if (q == '"' && line[i] == '\\') {
      tokens[i] = SyntaxToken::String;
      if (i + 1 < sz)
        tokens[i + 1] = SyntaxToken::String;
      i = std::min(i + 2, sz);
      continue;
    }
    if (line.compare(i, 3, triple) == 0) {
      tokens[i] = tokens[i + 1] = tokens[i + 2] = SyntaxToken::String;
      i += 3;
      // Up to two quotes directly before the delimiter are content
      // (`"""a""""` is `a"`), so the delimiter is the last three.
      for (int k = 0; k < 2 && i < sz && line[i] == q; ++k)
        tokens[i++] = SyntaxToken::String;
      return true;
    }
    tokens[i++] = SyntaxToken::String;
  }
  return false;
}

/**
 * @brief Try to parse a (possibly dotted, possibly quoted) key followed by
 *        `=` at @p i. On success tokenizes it and returns the index past
 *        the `=`; otherwise leaves @p tokens untouched.
 */
static std::optional<std::size_t> scanKey(std::string const& line, std::size_t i,
                                          std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  std::size_t j = i;
  auto skipBlank = [&] {
    while (j < sz && (line[j] == ' ' || line[j] == '\t'))
      ++j;
  };
  for (;;) {
    std::size_t s = j;
    SyntaxToken tok = SyntaxToken::Keyword;
    if (j < sz && (line[j] == '"' || line[j] == '\'')) {
      auto e = stringEnd(line, j);
      if (!e)
        break;
      j = *e;
      tok = SyntaxToken::String;
    } else {
      while (j < sz && isBareKeyChar(line[j]))
        ++j;
      if (j == s)
        break;
    }
    std::fill(tokens.begin() + s, tokens.begin() + j, tok);
    skipBlank();
    if (j < sz && line[j] == '.') {
      tokens[j++] = SyntaxToken::Operator;
      skipBlank();
      continue;
    }
    if (j < sz && line[j] == '=') {
      tokens[j] = SyntaxToken::Operator;
      return j + 1;
    }
    break;
  }
  std::fill(tokens.begin() + i, tokens.begin() + j, SyntaxToken::Normal);
  return std::nullopt;
}

/** @brief True if @p d is `YYYY-MM-DD` (the start of a date or date-time). */
static bool isDate(std::string_view d) {
  return d.size() >= 10 && d[4] == '-' && d[7] == '-' &&
         std::all_of(d.begin(), d.begin() + 10, [](char c) { return isDigit(c) || c == '-'; });
}

/**
 * @brief True if @p w is a TOML literal value: boolean, integer (decimal,
 *        `0x`/`0o`/`0b`, `_` separators), float (incl. `inf`/`nan`), or
 *        date/time. These share the numeric-literal colour.
 */
static bool isTomlLiteral(std::string_view w) {
  if (w == "true" || w == "false")
    return true;
  std::string_view v = w;
  if (!v.empty() && (v[0] == '+' || v[0] == '-'))
    v.remove_prefix(1);
  if (v == "inf" || v == "nan")
    return true;
  if (v.empty() || !isDigit(v[0]))
    return false;
  auto allOf = [](std::string_view s, auto pred) {
    return !s.empty() && std::all_of(s.begin(), s.end(), pred);
  };
  if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'o' || v[1] == 'b')) {
    char base = v[1];
    return allOf(v.substr(2), [base](char c) {
      if (c == '_')
        return true;
      if (base == 'x')
        return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      return base == 'o' ? c >= '0' && c <= '7' : c == '0' || c == '1';
    });
  }
  // Date / time (unsigned only): `YYYY-MM-DD...` or `HH:MM:SS...`.
  if (v.size() == w.size() && (isDate(v) || (v.size() >= 8 && v[2] == ':' && v[5] == ':')))
    return allOf(v, [](char c) {
      return isDigit(c) || std::string_view("-:.+TtZz ").find(c) != std::string_view::npos;
    });
  // Decimal integer / float: digits, optional fraction, optional exponent.
  std::size_t k = 0;
  auto digits = [&] {
    std::size_t s = k;
    while (k < v.size() && (isDigit(v[k]) || v[k] == '_'))
      ++k;
    return k > s;
  };
  if (!digits())
    return false;
  if (k < v.size() && v[k] == '.') {
    ++k;
    if (!digits())
      return false;
  }
  if (k < v.size() && (v[k] == 'e' || v[k] == 'E')) {
    ++k;
    if (k < v.size() && (v[k] == '+' || v[k] == '-'))
      ++k;
    if (!digits())
      return false;
  }
  return k == v.size();
}

void tokenizeTomlLine(std::string const& line, TmlSyntaxState& state,
                      std::vector<SyntaxToken>& tokens) {
  std::size_t sz = line.size();
  tokens.assign(sz, SyntaxToken::Normal);
  std::size_t i = 0;

  // Continuation of a multi-line string; whatever follows its close is
  // value context.
  bool expectKey = state.tomlDepth == 0;
  if (state.tomlTriple) {
    if (!scanMultiBody(line, i, state.tomlTriple, tokens))
      return;
    state.tomlTriple = 0;
    expectKey = false;
  }

  while (i < sz && (line[i] == ' ' || line[i] == '\t'))
    ++i;

  // Table header `[a.b]` / array-of-tables `[[a]]`: only at line start
  // outside any open array, where a `[` cannot begin a value.
  if (expectKey && i < sz && line[i] == '[') {
    tokens[i++] = SyntaxToken::Bracket;
    if (i < sz && line[i] == '[')
      tokens[i++] = SyntaxToken::Bracket;
    while (i < sz && line[i] != ']' && line[i] != '#') {
      char c = line[i];
      if (c == '"' || c == '\'') {
        std::size_t e = stringEnd(line, i).value_or(sz);
        std::fill(tokens.begin() + i, tokens.begin() + e, SyntaxToken::String);
        i = e;
      } else {
        if (c == '.')
          tokens[i] = SyntaxToken::Operator;
        else if (c != ' ' && c != '\t')
          tokens[i] = SyntaxToken::Preprocessor;
        ++i;
      }
    }
    for (int k = 0; k < 2 && i < sz && line[i] == ']'; ++k)
      tokens[i++] = SyntaxToken::Bracket;
    expectKey = false;
  }

  // Brackets opened on this line, innermost last; tells whether a `,`
  // separates inline-table entries (next comes a key) or array elements.
  // Brackets inherited from earlier lines are arrays (TOML 1.0 inline
  // tables are single-line).
  std::string open;
  while (i < sz) {
    char c = line[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '#') {
      std::fill(tokens.begin() + i, tokens.end(), SyntaxToken::Comment);
      break;
    }
    if (expectKey) {
      expectKey = false;
      if (auto e = scanKey(line, i, tokens)) {
        i = *e;
        continue;
      }
    }
    if (c == '"' || c == '\'') {
      if (line.compare(i, 3, std::string(3, c)) == 0) {
        std::fill_n(tokens.begin() + i, 3, SyntaxToken::String);
        i += 3;
        if (!scanMultiBody(line, i, c, tokens)) {
          state.tomlTriple = c;
          return;
        }
      } else {
        std::size_t e = stringEnd(line, i).value_or(sz);
        std::fill(tokens.begin() + i, tokens.begin() + e, SyntaxToken::String);
        i = e;
      }
      continue;
    }
    if (c == '[' || c == '{') {
      tokens[i++] = c == '[' ? SyntaxToken::Bracket : SyntaxToken::Brace;
      open += c;
      ++state.tomlDepth;
      expectKey = c == '{';
      continue;
    }
    if (c == ']' || c == '}') {
      tokens[i++] = c == ']' ? SyntaxToken::Bracket : SyntaxToken::Brace;
      if (!open.empty())
        open.pop_back();
      if (state.tomlDepth > 0)
        --state.tomlDepth;
      continue;
    }
    if (c == ',' || c == '=') {
      tokens[i++] = SyntaxToken::Operator;
      expectKey = c == ',' && !open.empty() && open.back() == '{';
      continue;
    }
    // Bare word: number, boolean, date/time (or invalid TOML). The first
    // character is none of the stop characters (handled above), so this
    // always makes progress.
    auto isStop = [](char ch) {
      return std::string_view(" \t#,=[]{}\"'").find(ch) != std::string_view::npos;
    };
    std::size_t s = i;
    while (i < sz && !isStop(line[i]))
      ++i;
    // A date-time may separate date and time with a space.
    if (i + 3 < sz && line[i] == ' ' && isDate(std::string_view(line).substr(s, i - s)) &&
        isDigit(line[i + 1]) && isDigit(line[i + 2]) && line[i + 3] == ':') {
      ++i;
      while (i < sz && !isStop(line[i]))
        ++i;
    }
    if (isTomlLiteral(std::string_view(line).substr(s, i - s)))
      std::fill(tokens.begin() + s, tokens.begin() + i, SyntaxToken::Number);
  }
}
