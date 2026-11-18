#include "regex.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <tuple>

namespace {

/** @brief Units at or above this are invalid UTF-8 bytes (kInvalidUnit + byte). */
constexpr std::uint32_t kInvalidUnit = 0x110000;

/** @brief The unit at byte @p pos of @p s and its length: a code point, or
 *         kInvalidUnit + the byte for a byte that is not valid UTF-8. */
std::pair<std::uint32_t, std::size_t> unitAt(std::string_view s, std::size_t pos) {
  auto [cp, len] = decodeUtf8(s.data() + pos, s.size() - pos);
  if (len == 1 && static_cast<unsigned char>(s[pos]) >= 0x80)
    return {kInvalidUnit + static_cast<unsigned char>(s[pos]), 1};
  return {cp, len};
}

/** @brief `\w`: an ASCII letter, digit or underscore. */
bool isWordByte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/** @brief A word boundary at byte @p pos of @p line (ASCII `\w`, as ECMAScript). */
bool atWordBoundary(std::string_view line, std::size_t pos) {
  bool before = pos > 0 && isWordByte(line[pos - 1]);
  bool after = pos < line.size() && isWordByte(line[pos]);
  return before != after;
}

using Ranges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

/** @brief Sort and merge @p r into disjoint, non-adjacent ranges. */
Ranges normalized(Ranges r) {
  std::sort(r.begin(), r.end());
  Ranges out;
  for (auto const& [lo, hi] : r) {
    if (!out.empty() && lo <= out.back().second + 1)
      out.back().second = std::max(out.back().second, hi);
    else
      out.emplace_back(lo, hi);
  }
  return out;
}

/** @brief The code points (up to U+10FFFF) not in @p r. */
Ranges complement(Ranges const& r) {
  Ranges out;
  std::uint32_t next = 0;
  for (auto const& [lo, hi] : normalized(r)) {
    if (lo > next)
      out.emplace_back(next, lo - 1);
    next = hi + 1;
  }
  if (next <= kMaxCodepoint)
    out.emplace_back(next, kMaxCodepoint);
  return out;
}

Ranges const kDigit = {{'0', '9'}};
Ranges const kWord = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
Ranges const kSpace = {{'\t', '\r'}, {' ', ' '}}; // \t \n \v \f \r and space

/** @brief The ASCII POSIX bracket classes, `[[:name:]]`. */
std::optional<Ranges> posixClass(std::string_view name) {
  if (name == "alpha")
    return Ranges{{'A', 'Z'}, {'a', 'z'}};
  if (name == "digit")
    return kDigit;
  if (name == "alnum")
    return Ranges{{'0', '9'}, {'A', 'Z'}, {'a', 'z'}};
  if (name == "upper")
    return Ranges{{'A', 'Z'}};
  if (name == "lower")
    return Ranges{{'a', 'z'}};
  if (name == "space")
    return kSpace;
  if (name == "punct")
    return Ranges{{'!', '/'}, {':', '@'}, {'[', '`'}, {'{', '~'}};
  if (name == "xdigit")
    return Ranges{{'0', '9'}, {'A', 'F'}, {'a', 'f'}};
  if (name == "word")
    return kWord;
  return std::nullopt;
}

/** @brief A pattern error: the message, and the 1-based character position. */
struct PatternError {
  std::string message;
  std::size_t at;
};

/** @brief Parsed pattern (an AST). */
struct Node {
  enum class Kind {
    Empty,
    Char,
    Any,
    Class,
    Bol,
    Eol,
    WordB,
    NotWordB,
    Group,
    Concat,
    Alt,
    Repeat
  };
  Kind kind = Kind::Empty;
  std::uint32_t cp = 0; // Char
  std::size_t cls = 0;  // Class: index into the class table
  int capture = -1;     // Group: its number, -1 if non-capturing
  std::size_t min = 0;  // Repeat
  std::size_t max = 0;  // Repeat; ignored if unbounded
  bool unbounded = false;
  bool greedy = true;
  std::vector<Node> kids; // Group, Repeat: one; Concat, Alt: any number
};

bool isAssertion(Node const& n) {
  return n.kind == Node::Kind::Bol || n.kind == Node::Kind::Eol || n.kind == Node::Kind::WordB ||
         n.kind == Node::Kind::NotWordB;
}

bool isAsciiPunct(std::uint32_t c) {
  return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') || (c >= '[' && c <= '`') ||
         (c >= '{' && c <= '~');
}

std::optional<std::uint32_t> hexValue(std::uint32_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return std::nullopt;
}

/** @brief Recursive-descent parser; recursion is bounded by kMaxNesting. */
class Parser {
public:
  Parser(std::vector<std::uint32_t> pattern, std::vector<Regex::CharClass>& classes)
      : _p(std::move(pattern)), _classes(classes) {}

  Node parse() {
    Node n = alternation();
    if (_i < _p.size()) // only a ')' stops alternation() early
      fail("unmatched ')'");
    return n;
  }

  std::size_t groups() const { return _groups; }

private:
  std::vector<std::uint32_t> _p;
  std::vector<Regex::CharClass>& _classes;
  std::size_t _i = 0;
  std::size_t _groups = 0;
  std::size_t _depth = 0;

  [[noreturn]] void fail(std::string message) const {
    throw PatternError{std::move(message), _i + 1};
  }
  [[noreturn]] void failAt(std::string message, std::size_t i) const {
    throw PatternError{std::move(message), i + 1};
  }
  bool atEnd() const { return _i >= _p.size(); }
  std::uint32_t peek(std::size_t ahead = 0) const {
    return _i + ahead < _p.size() ? _p[_i + ahead] : 0xFFFFFFFF;
  }

  Node classNode(Ranges ranges, bool negated) {
    _classes.push_back({normalized(std::move(ranges)), negated});
    Node n{.kind = Node::Kind::Class};
    n.cls = _classes.size() - 1;
    return n;
  }

  Node alternation() {
    std::vector<Node> alts;
    alts.push_back(concatenation());
    while (!atEnd() && peek() == '|') {
      ++_i;
      alts.push_back(concatenation());
    }
    if (alts.size() == 1)
      return std::move(alts[0]);
    return Node{.kind = Node::Kind::Alt, .kids = std::move(alts)};
  }

  Node concatenation() {
    std::vector<Node> items;
    while (!atEnd() && peek() != '|' && peek() != ')')
      items.push_back(quantified());
    if (items.empty())
      return Node{};
    if (items.size() == 1)
      return std::move(items[0]);
    return Node{.kind = Node::Kind::Concat, .kids = std::move(items)};
  }

  /** @brief A `{n}`, `{n,}` or `{n,m}` at _i, or nullopt (and _i unchanged)
   *         if what follows is not one. */
  std::optional<std::pair<std::size_t, std::optional<std::size_t>>> braces() {
    std::size_t j = _i + 1;
    auto number = [&]() -> std::optional<std::size_t> {
      std::size_t start = j, v = 0;
      while (j < _p.size() && _p[j] >= '0' && _p[j] <= '9') {
        v = std::min<std::size_t>(v * 10 + (_p[j] - '0'), kMaxRepeatSentinel);
        ++j;
      }
      return j > start ? std::optional{v} : std::nullopt;
    };
    auto n = number();
    if (!n)
      return std::nullopt;
    std::optional<std::size_t> m = n;
    if (j < _p.size() && _p[j] == ',') {
      ++j;
      m = number(); // absent: unbounded
    }
    if (j >= _p.size() || _p[j] != '}')
      return std::nullopt;
    std::size_t start = _i;
    _i = j + 1;
    if (*n > Regex::kMaxRepeat || (m && *m > Regex::kMaxRepeat))
      failAt("repeat count over " + std::to_string(Regex::kMaxRepeat), start);
    if (m && *m < *n)
      failAt("repeat range out of order", start);
    if (m)
      return std::pair{*n, m};
    return std::pair{*n, std::optional<std::size_t>{}};
  }
  static constexpr std::size_t kMaxRepeatSentinel = 1'000'000;

  Node quantified() {
    Node atom = this->atom();
    for (bool first = true; !atEnd(); first = false) {
      std::uint32_t c = peek();
      std::size_t min = 0, max = 0;
      bool unbounded = false;
      std::size_t at = _i;
      if (c == '*') {
        unbounded = true;
        ++_i;
      } else if (c == '+') {
        min = 1;
        unbounded = true;
        ++_i;
      } else if (c == '?') {
        max = 1;
        ++_i;
      } else if (c == '{') {
        auto b = braces();
        if (!b)
          fail("'{' starts no repeat count; write \\{ for a literal '{'");
        min = b->first;
        unbounded = !b->second;
        max = b->second.value_or(0);
      } else {
        break;
      }
      if (!first || isAssertion(atom))
        failAt("nothing to repeat", at);
      bool greedy = true;
      if (!atEnd() && peek() == '?') {
        greedy = false;
        ++_i;
      }
      Node rep{.kind = Node::Kind::Repeat,
               .min = min,
               .max = max,
               .unbounded = unbounded,
               .greedy = greedy};
      rep.kids.push_back(std::move(atom));
      atom = std::move(rep);
    }
    return atom;
  }

  Node atom() {
    std::uint32_t c = peek();
    switch (c) {
    case '(':
      return group();
    case '[':
      return bracket();
    case '.':
      ++_i;
      return Node{.kind = Node::Kind::Any};
    case '^':
      ++_i;
      return Node{.kind = Node::Kind::Bol};
    case '$':
      ++_i;
      return Node{.kind = Node::Kind::Eol};
    case '\\':
      return escape();
    case '*':
    case '+':
    case '?':
      fail("nothing to repeat");
    case '{':
      if (braces())
        failAt("nothing to repeat", _i);
      fail("'{' starts no repeat count; write \\{ for a literal '{'");
    default:
      ++_i;
      return Node{.kind = Node::Kind::Char, .cp = c};
    }
  }

  Node group() {
    std::size_t open = _i;
    ++_i;
    int capture = -1;
    if (peek() == '?') {
      if (peek(1) == ':') {
        _i += 2;
      } else if (peek(1) == '=' || peek(1) == '!' ||
                 (peek(1) == '<' && (peek(2) == '=' || peek(2) == '!'))) {
        failAt("lookahead and lookbehind are not supported", open);
      } else if (peek(1) == '<') {
        failAt("named groups are not supported", open);
      } else {
        failAt("unknown group syntax after '(?'", open);
      }
    } else {
      capture = static_cast<int>(++_groups);
    }
    if (++_depth > Regex::kMaxNesting)
      failAt("groups nested too deeply", open);
    Node inner = alternation();
    --_depth;
    if (atEnd())
      failAt("unmatched '('", open);
    ++_i; // ')'
    Node g{.kind = Node::Kind::Group, .capture = capture};
    g.kids.push_back(std::move(inner));
    return g;
  }

  /** @brief A code point written as an escape after the backslash at _i-1
   *         (`\t`, `\xHH`, `\u…`, or punctuation), or nullopt if the escape
   *         is not one of those. Advances past it. */
  std::optional<std::uint32_t> charEscape() {
    std::size_t start = _i - 1;
    std::uint32_t c = peek();
    switch (c) {
    case 't':
      ++_i;
      return '\t';
    case 'n':
      ++_i;
      return '\n';
    case 'r':
      ++_i;
      return '\r';
    case 'f':
      ++_i;
      return '\f';
    case 'v':
      ++_i;
      return '\v';
    case 'x': {
      auto h1 = hexValue(peek(1)), h2 = hexValue(peek(2));
      if (!h1 || !h2)
        failAt("\\x needs two hex digits", start);
      _i += 3;
      return *h1 * 16 + *h2;
    }
    case 'u': {
      std::uint32_t v = 0;
      if (peek(1) == '{') {
        std::size_t j = _i + 2, digits = 0;
        while (j < _p.size() && hexValue(_p[j]) && digits < 7) {
          v = v * 16 + *hexValue(_p[j]);
          ++j;
          ++digits;
        }
        if (digits == 0 || digits > 6 || j >= _p.size() || _p[j] != '}')
          failAt("\\u{…} needs 1 to 6 hex digits", start);
        _i = j + 1;
      } else {
        for (std::size_t k = 1; k <= 4; ++k) {
          auto h = hexValue(peek(k));
          if (!h)
            failAt("\\u needs four hex digits or braces", start);
          v = v * 16 + *h;
        }
        _i += 5;
      }
      if (v > kMaxCodepoint || isSurrogate(v))
        failAt("\\u names no character", start);
      return v;
    }
    default:
      if (c < 0x80 && isAsciiPunct(c)) {
        ++_i;
        return c;
      }
      if (c >= 0x80 || c == ' ') { // not a letter or digit: literal
        ++_i;
        return c;
      }
      return std::nullopt;
    }
  }

  /** @brief The set for `\d \D \w \W \s \S` at _i (after the backslash),
   *         or nullopt. Advances past it. */
  std::optional<std::pair<Ranges, bool>> setEscape() {
    std::uint32_t c = peek();
    Ranges const* r = c == 'd' || c == 'D'   ? &kDigit
                      : c == 'w' || c == 'W' ? &kWord
                      : c == 's' || c == 'S' ? &kSpace
                                             : nullptr;
    if (!r)
      return std::nullopt;
    ++_i;
    return std::pair{*r, c == 'D' || c == 'W' || c == 'S'};
  }

  [[noreturn]] void badEscape(std::size_t start) {
    std::uint32_t c = peek();
    if (c >= '1' && c <= '9')
      failAt("back-references are not supported", start);
    if (atEnd())
      failAt("pattern ends with a backslash", start);
    std::string name = "\\";
    name += static_cast<char>(c);
    failAt("unknown escape " + name, start);
  }

  Node escape() {
    std::size_t start = _i;
    ++_i;
    if (atEnd())
      failAt("pattern ends with a backslash", start);
    std::uint32_t c = peek();
    if (c == 'b' || c == 'B') {
      ++_i;
      return Node{.kind = c == 'b' ? Node::Kind::WordB : Node::Kind::NotWordB};
    }
    if (auto set = setEscape())
      return classNode(std::move(set->first), set->second);
    if (auto ch = charEscape())
      return Node{.kind = Node::Kind::Char, .cp = *ch};
    badEscape(start);
  }

  /** @brief One item of a bracket class: a code point, or a set. */
  struct ClassItem {
    std::optional<std::uint32_t> cp;
    Ranges set;
  };

  ClassItem classItem(std::size_t open) {
    if (atEnd())
      failAt("unterminated character class (missing ']')", open);
    std::uint32_t c = peek();
    if (c == '[' && peek(1) == ':') {
      std::size_t start = _i, j = _i + 2;
      std::string name;
      while (j + 1 < _p.size() && !(_p[j] == ':' && _p[j + 1] == ']')) {
        if (_p[j] >= 0x80)
          break;
        name += static_cast<char>(_p[j]);
        ++j;
      }
      if (j + 1 < _p.size() && _p[j] == ':' && _p[j + 1] == ']') {
        auto r = posixClass(name);
        if (!r)
          failAt("unknown POSIX class [:" + name + ":]", start);
        _i = j + 2;
        return {std::nullopt, *r};
      }
      // Not a POSIX class: an ordinary '['.
    }
    if (c == '\\') {
      std::size_t start = _i;
      ++_i;
      if (atEnd())
        failAt("unterminated character class (missing ']')", open);
      if (peek() == 'b')
        failAt("\\b is not allowed in a character class", start);
      if (auto set = setEscape())
        return {std::nullopt, set->second ? complement(set->first) : set->first};
      if (auto ch = charEscape())
        return {*ch, {}};
      badEscape(start);
    }
    ++_i;
    return {c, {}};
  }

  Node bracket() {
    std::size_t open = _i;
    ++_i;
    bool negated = false;
    if (peek() == '^') {
      negated = true;
      ++_i;
    }
    Ranges ranges;
    bool first = true;
    for (;;) {
      if (atEnd())
        failAt("unterminated character class (missing ']')", open);
      if (peek() == ']' && !first) {
        ++_i;
        break;
      }
      first = false;
      std::size_t itemAt = _i;
      ClassItem lo = classItem(open);
      if (lo.cp && peek() == '-' && peek(1) != ']' && _i + 1 < _p.size()) {
        ++_i; // '-'
        ClassItem hi = classItem(open);
        if (!hi.cp)
          failAt("a range cannot end in a class escape", itemAt);
        if (*hi.cp < *lo.cp)
          failAt("range out of order", itemAt);
        ranges.emplace_back(*lo.cp, *hi.cp);
      } else if (lo.cp) {
        ranges.emplace_back(*lo.cp, *lo.cp);
      } else {
        ranges.insert(ranges.end(), lo.set.begin(), lo.set.end());
      }
    }
    return classNode(std::move(ranges), negated);
  }
};

} // namespace

/** @brief Turns the AST into Pike VM instructions. */
struct RegexBuilder {
  Regex& re;

  std::size_t emit(Regex::Inst inst) {
    if (re._prog.size() >= Regex::kMaxProgram)
      throw PatternError{"pattern too large", 1};
    re._prog.push_back(inst);
    return re._prog.size() - 1;
  }
  std::uint32_t here() const { return static_cast<std::uint32_t>(re._prog.size()); }

  void gen(Node const& n) {
    using K = Node::Kind;
    using Op = Regex::Op;
    switch (n.kind) {
    case K::Empty:
      break;
    case K::Char:
      emit({Op::Char, n.cp});
      break;
    case K::Any:
      emit({Op::Any});
      break;
    case K::Class:
      emit({Op::Class, static_cast<std::uint32_t>(n.cls)});
      break;
    case K::Bol:
      emit({Op::Bol});
      break;
    case K::Eol:
      emit({Op::Eol});
      break;
    case K::WordB:
      emit({Op::WordB});
      break;
    case K::NotWordB:
      emit({Op::NotWordB});
      break;
    case K::Group: {
      bool saved = n.capture > 0 && static_cast<std::size_t>(n.capture) < Regex::kReportedGroups;
      if (saved)
        emit({Op::Save, static_cast<std::uint32_t>(2 * n.capture)});
      gen(n.kids[0]);
      if (saved)
        emit({Op::Save, static_cast<std::uint32_t>(2 * n.capture + 1)});
      break;
    }
    case K::Concat:
      for (auto const& k : n.kids)
        gen(k);
      break;
    case K::Alt: {
      // Split(alt1, next); alt1; Jmp end; next: Split(alt2, next2); ...; altN; end:
      std::vector<std::size_t> jumps;
      for (std::size_t i = 0; i < n.kids.size(); ++i) {
        if (i + 1 < n.kids.size()) {
          std::size_t split = emit({Op::Split});
          re._prog[split].x = here();
          gen(n.kids[i]);
          jumps.push_back(emit({Op::Jmp}));
          re._prog[split].y = here();
        } else {
          gen(n.kids[i]);
        }
      }
      for (auto j : jumps)
        re._prog[j].x = here();
      break;
    }
    case K::Repeat: {
      Node const& body = n.kids[0];
      for (std::size_t i = 0; i < n.min; ++i)
        gen(body);
      // Split's x is the preferred branch: the body when greedy, the way
      // out when lazy.
      auto setSplit = [&](std::size_t split, std::uint32_t bodyPc, std::uint32_t outPc) {
        re._prog[split].x = n.greedy ? bodyPc : outPc;
        re._prog[split].y = n.greedy ? outPc : bodyPc;
      };
      if (n.unbounded) {
        std::size_t split = emit({Op::Split});
        std::uint32_t bodyPc = here();
        gen(body);
        emit({Op::Jmp, static_cast<std::uint32_t>(split)});
        setSplit(split, bodyPc, here());
      } else if (n.max > n.min) {
        // Nested optional copies: Split(b1, end); b1; Split(b2, end); b2; ... end:
        std::vector<std::size_t> splits;
        for (std::size_t i = n.min; i < n.max; ++i) {
          splits.push_back(emit({Op::Split}));
          gen(body);
        }
        for (auto s : splits)
          setSplit(s, s + 1, here());
      }
      break;
    }
    }
  }

  /**
   * @brief Append to @p out, as UTF-8, the literal text every match of
   *        @p n starts with.
   * @return True if @p n matches exactly that text (zero-width assertions
   *         aside), so that what follows @p n continues the prefix.
   */
  static bool literalPrefix(Node const& n, std::string& out) {
    using K = Node::Kind;
    switch (n.kind) {
    case K::Char: {
      auto [len, bytes] = encodeUtf8(n.cp);
      out.append(reinterpret_cast<char const*>(bytes.data()), len);
      return true;
    }
    case K::Empty:
    case K::Bol:
    case K::Eol:
    case K::WordB:
    case K::NotWordB:
      return true; // nothing consumed: the match starts where the text after it does
    case K::Group:
      return literalPrefix(n.kids[0], out);
    case K::Concat:
      for (auto const& k : n.kids)
        if (!literalPrefix(k, out))
          return false;
      return true;
    case K::Repeat:
      if (n.min > 0) // the first repetition is always there
        literalPrefix(n.kids[0], out);
      return false;
    default:
      return false;
    }
  }

  /**
   * @brief The longest run of literal text that every match contains: the
   *        longest sequence of consecutive Char nodes among the items that
   *        are always matched exactly once (top-level concatenation, and
   *        groups in it).
   */
  static std::string requiredLiteral(Node const& root) {
    std::string best, run;
    auto flush = [&] {
      if (run.size() > best.size())
        best = run;
      run.clear();
    };
    auto walk = [&](auto&& self, Node const& n) -> void {
      using K = Node::Kind;
      switch (n.kind) {
      case K::Char: {
        auto [len, bytes] = encodeUtf8(n.cp);
        run.append(reinterpret_cast<char const*>(bytes.data()), len);
        break;
      }
      case K::Empty:
      case K::Bol:
      case K::Eol:
      case K::WordB:
      case K::NotWordB:
        break; // zero-width: the run goes on across it
      case K::Group:
        self(self, n.kids[0]);
        break;
      case K::Concat:
        for (auto const& k : n.kids)
          self(self, k);
        break;
      default:
        flush();
      }
    };
    walk(walk, root);
    flush();
    return best;
  }
};

bool Regex::CharClass::contains(std::uint32_t unit) const {
  if (unit >= kInvalidUnit)
    return negated;
  auto it = std::upper_bound(ranges.begin(), ranges.end(), unit,
                             [](std::uint32_t u, auto const& r) { return u < r.first; });
  bool in = it != ranges.begin() && unit <= std::prev(it)->second;
  return in != negated;
}

std::expected<Regex, std::string> Regex::compile(std::string_view pattern) {
  if (pattern.empty())
    return std::unexpected("empty pattern");
  std::vector<std::uint32_t> cps;
  for (std::size_t i = 0; i < pattern.size();) {
    auto [u, len] = unitAt(pattern, i);
    if (u >= kInvalidUnit)
      return std::unexpected("invalid UTF-8 in the pattern (at " + std::to_string(cps.size() + 1) +
                             ")");
    cps.push_back(u);
    i += len;
  }
  Regex re;
  try {
    Parser parser(std::move(cps), re._classes);
    Node root = parser.parse();
    re._groups = parser.groups();
    re._slots = 2 * std::min(re._groups + 1, kReportedGroups);
    RegexBuilder builder{re};
    builder.emit({Op::Save, 0});
    builder.gen(root);
    builder.emit({Op::Save, 1});
    builder.emit({Op::Match});
    RegexBuilder::literalPrefix(root, re._prefix);
    re._required = RegexBuilder::requiredLiteral(root);
    if (re._required.size() <= re._prefix.size())
      re._required.clear(); // the prefix search already covers it
  } catch (PatternError const& e) {
    return std::unexpected(e.message + " (at " + std::to_string(e.at) + ")");
  }
  return re;
}

std::optional<Regex::Match> Regex::search(std::string_view line, std::size_t from) const {
  if (from > line.size())
    return std::nullopt;
  return run(line, from, false);
}

std::optional<Regex::Match> Regex::matchAt(std::string_view line, std::size_t at) const {
  if (at > line.size())
    return std::nullopt;
  return run(line, at, true);
}

std::optional<Regex::Match> Regex::run(std::string_view line, std::size_t from,
                                       bool anchored) const {
  // Every match contains _required: a line without it has none.
  if (!_required.empty() && line.find(_required, from) == npos)
    return std::nullopt;
  // Every match starts with _prefix: skip to where it occurs.
  std::size_t pos = from;
  if (!_prefix.empty()) {
    if (anchored) {
      if (line.substr(from).substr(0, _prefix.size()) != _prefix)
        return std::nullopt;
    } else {
      pos = line.find(_prefix, from);
      if (pos == npos)
        return std::nullopt;
    }
  }

  auto& s = _scratch;
  std::size_t const ns = _slots;
  if (s.mark.size() != _prog.size()) {
    s.mark.assign(_prog.size(), 0);
    s.gen = 0;
  }
  auto newGeneration = [&] {
    if (++s.gen == 0) { // wrapped: forget every old mark
      std::fill(s.mark.begin(), s.mark.end(), 0);
      s.gen = 1;
    }
  };
  s.cur.assign(ns, npos);

  // Follow the instructions that consume nothing from pc0 at pos and add
  // the threads they lead to (consuming instructions and Match) to list,
  // in priority order. Iterative, with capture writes undone on the way
  // back, so a long chain of Splits cannot overflow the stack.
  auto addThread = [&](Scratch::List& list, std::uint32_t pc0, std::size_t at) {
    s.stack.clear();
    s.stack.push_back({pc0, -1, 0});
    while (!s.stack.empty()) {
      auto f = s.stack.back();
      s.stack.pop_back();
      if (f.slot >= 0) {
        s.cur[static_cast<std::size_t>(f.slot)] = f.old;
        continue;
      }
      std::uint32_t pc = f.pc;
      for (;;) {
        if (s.mark[pc] == s.gen)
          break;
        s.mark[pc] = s.gen;
        Inst const& in = _prog[pc];
        bool follow = true;
        switch (in.op) {
        case Op::Jmp:
          pc = in.x;
          continue;
        case Op::Split:
          s.stack.push_back({in.y, -1, 0});
          pc = in.x;
          continue;
        case Op::Save:
          if (in.x < ns) {
            s.stack.push_back({0, static_cast<std::int32_t>(in.x), s.cur[in.x]});
            s.cur[in.x] = at;
          }
          ++pc;
          continue;
        case Op::Bol:
          follow = at == 0;
          break;
        case Op::Eol:
          follow = at == line.size();
          break;
        case Op::WordB:
          follow = atWordBoundary(line, at);
          break;
        case Op::NotWordB:
          follow = !atWordBoundary(line, at);
          break;
        default: // consuming, or Match: a thread
          list.pcs.push_back(pc);
          list.caps.insert(list.caps.end(), s.cur.begin(), s.cur.end());
          follow = false;
          break;
        }
        if (!follow)
          break;
        ++pc;
      }
    }
  };

  Scratch::List* clist = &s.a;
  Scratch::List* nlist = &s.b;
  clist->pcs.clear();
  clist->caps.clear();
  newGeneration();
  std::optional<Match> best;
  for (;;) {
    // A new thread at the lowest priority starts a match here, until one
    // is found: the leftmost match wins.
    if (!best && (!anchored || pos == from)) {
      std::fill(s.cur.begin(), s.cur.end(), npos);
      addThread(*clist, 0, pos);
    }
    if (clist->pcs.empty()) {
      if (best || anchored || pos >= line.size())
        break;
      pos += unitAt(line, pos).second;
      if (!_prefix.empty()) {
        pos = line.find(_prefix, pos);
        if (pos == npos)
          break;
      }
      newGeneration(); // clist is empty: a fresh one for the next start
      continue;
    }

    std::uint32_t unit = 0;
    std::size_t len = 0;
    bool more = pos < line.size();
    if (more)
      std::tie(unit, len) = unitAt(line, pos);

    nlist->pcs.clear();
    nlist->caps.clear();
    newGeneration();
    for (std::size_t t = 0; t < clist->pcs.size(); ++t) {
      Inst const& in = _prog[clist->pcs[t]];
      std::size_t const* caps = clist->caps.data() + t * ns;
      bool step = false;
      switch (in.op) {
      case Op::Match: {
        Match m;
        for (std::size_t g = 0; g < ns / 2; ++g)
          if (caps[2 * g] != npos && caps[2 * g + 1] != npos)
            m.groups[g] = {caps[2 * g], caps[2 * g + 1]};
        best = m;
        // Lower-priority threads lose to this match.
        t = clist->pcs.size();
        continue;
      }
      case Op::Char:
        step = more && unit == in.x;
        break;
      case Op::Any:
        step = more;
        break;
      case Op::Class:
        step = more && _classes[in.x].contains(unit);
        break;
      default:
        break;
      }
      if (step) {
        std::copy(caps, caps + ns, s.cur.begin());
        addThread(*nlist, clist->pcs[t] + 1, pos + len);
      }
    }
    std::swap(clist, nlist);
    if (!more)
      break;
    pos += len;
  }
  return best;
}

std::string Regex::expand(std::string_view replacement, std::string_view line, Match const& m) {
  std::string out;
  out.reserve(replacement.size());
  auto group = [&](std::size_t g) {
    if (m.groups[g].matched())
      out.append(line.substr(m.groups[g].begin, m.groups[g].end - m.groups[g].begin));
  };
  for (std::size_t i = 0; i < replacement.size(); ++i) {
    char c = replacement[i];
    if (c != '$' || i + 1 >= replacement.size()) {
      out += c;
      continue;
    }
    char n = replacement[i + 1];
    if (n == '&') {
      group(0);
    } else if (n >= '0' && n <= '9') {
      group(static_cast<std::size_t>(n - '0'));
    } else if (n == '$') {
      out += '$';
    } else if (n == '`') {
      out.append(line.substr(0, m.begin()));
    } else if (n == '\'') {
      out.append(line.substr(m.end()));
    } else {
      out += c; // a literal '$'; the next character is read on its own
      continue;
    }
    ++i;
  }
  return out;
}
