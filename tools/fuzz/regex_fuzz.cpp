// Fuzzer for Regex (src/regex.hpp), cx's own regex engine.
//  1. Differential against std::regex (ECMAScript) on patterns of the subset
//     both accept, on ASCII lines (where bytes and code points agree):
//     group 0 and every group outside a repetition must agree, for searches
//     from any start offset. Quantifiers are only applied to items that
//     cannot match empty (libstdc++ does not implement ECMAScript's rule
//     for empty iterations) and groups inside repetitions are not compared
//     (ECMAScript resets them per iteration, libstdc++ and Regex do not).
//  2. Over lines: searchLines() on random lines finds what search() finds
//     in the same lines joined by '\n', for patterns with \n but no ^ $.
//  3. Robustness: random bytes as a pattern compile or are refused; on
//     random UTF-8 lines a match starts at or after its search offset,
//     lies on code-point boundaries, and matchAt() at its start gives it
//     again.
//  4. Time: random patterns, nullable loops included, on long lines must
//     stay fast; anything over SLOW_MS (default 2000) is reported.
// usage: regex_fuzz SEED ITERS
#include "regex.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <random>
#include <regex>
#include <string>
#include <unistd.h>
#include <vector>

static std::mt19937_64 rng;
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }

static void onAbort(int) {
  void* bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
  _exit(134);
}

static std::string esc(std::string_view s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || c >= 0x7f) {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  return o;
}

static unsigned long long gSeed;
[[noreturn]] static void fail(std::string const& what) {
  std::fprintf(stderr, "FAIL (seed %llu): %s\n", gSeed, what.c_str());
  std::exit(3);
}

// ------------------------------------------------------- pattern generator
struct Gen {
  bool lineBreaks = false; // may use \n (for overLines())
  bool anchors = true;     // may use ^ and $
  int groups = 0;
  std::vector<bool> inRepeat; // per group number (index 0 unused)
  int repeatDepth = 0;

  struct Piece {
    std::string text;
    bool nullable;
  };

  Piece atom(int depth) {
    switch (R(depth > 2 ? 9 : 12)) {
    case 0:
    case 1:
    case 2:
      return {std::string(1, "abc"[R(3)]), false};
    case 3:
      return {".", false};
    case 4: {
      static char const* cls[] = {"[ab]",   "[^a]",        "[a-c]",        "[^ab ]",      "\\d",
                                  "\\w",    "\\s",         "\\W",          "\\D",         "\\S",
                                  "[\\w-]", "[[:alpha:]]", "[[:digit:]_]", "[^[:space:]]"};
      return {cls[R(std::size(cls))], false};
    }
    case 5: {
      static char const* zw[] = {"\\b", "\\B", "^", "$"};
      return {zw[R(anchors ? 4 : 2)], true};
    }
    case 6:
      if (lineBreaks && R(2))
        return {R(3) ? "\\n" : "[\\n ]", false};
      return {std::string(1, " _1-"[R(4)]), false};
    case 7:
      return {"\\-", false};
    case 8:
      return {R(2) ? "x" : "\\.", false};
    default: {
      // a group
      bool capture = R(3) != 0;
      int num = 0;
      if (capture) {
        num = ++groups;
        inRepeat.resize(num + 1);
        inRepeat[num] = repeatDepth > 0;
      }
      Piece in = alternation(depth + 1);
      return {(capture ? "(" : "(?:") + in.text + ")", in.nullable};
    }
    }
  }

  Piece quantified(int depth) {
    // Decide on a quantifier first, so groups inside know they repeat.
    int q = static_cast<int>(R(10));
    bool rep = q >= 6;
    if (rep)
      ++repeatDepth;
    Piece a = atom(depth);
    if (rep)
      --repeatDepth;
    if (!rep || a.nullable)
      return a; // ECMAScript's empty-iteration rule: keep out
    static char const* qs[] = {"*", "+", "?", "{2}", "{1,3}", "{2,}", "{0,2}"};
    std::string quant = qs[R(std::size(qs))];
    if (R(3) == 0)
      quant += '?';
    bool nullable = quant[0] == '*' || quant[0] == '?' || quant.starts_with("{0");
    return {a.text + quant, nullable};
  }

  Piece concat(int depth) {
    Piece out{"", true};
    for (std::size_t n = 1 + R(depth > 2 ? 2 : 4); n; --n) {
      Piece p = quantified(depth);
      out.text += p.text;
      out.nullable = out.nullable && p.nullable;
    }
    return out;
  }

  Piece alternation(int depth) {
    Piece out = concat(depth);
    while (R(depth > 1 ? 6 : 3) == 0) {
      Piece p = R(8) == 0 ? Piece{"", true} : concat(depth);
      out.text += "|" + p.text;
      out.nullable = out.nullable || p.nullable;
    }
    return out;
  }
};

static std::string randAsciiLine(std::size_t n) {
  static char const alpha[] = "abcabcx _1-.";
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s += alpha[R(sizeof alpha - 1)];
  return s;
}

static std::string randUtf8Line(std::size_t n) {
  static char const* bits[] = {
      "a",        "b",    " ",    "_",    "1", "\xc3\xa9", "\xe4\xb8\xad", "\xf0\x9f\x98\x80",
      "\xcc\x81", "\xff", "\xc3", "\x80", "-", "."};
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s += bits[R(std::size(bits))];
  return s;
}

static bool boundary(std::string_view s, std::size_t off) {
  return off == 0 || off >= s.size() || snapToCodepoint(s, off) == off;
}

// ------------------------------------------------------------ differential
static void differential() {
  Gen g;
  std::string pat = g.alternation(0).text;
  if (pat.empty())
    return;
  auto ours = Regex::compile(pat);
  std::regex theirs;
  try {
    theirs = std::regex(pat, std::regex::ECMAScript);
  } catch (std::regex_error const&) {
    if (ours)
      fail("std::regex refuses /" + pat + "/, Regex accepts it");
    return;
  }
  if (!ours)
    fail("Regex refuses /" + pat + "/: " + ours.error());
  for (int k = 0; k < 8; ++k) {
    std::string const line = randAsciiLine(R(14));
    std::size_t from = R(line.size() + 1);
    auto m = ours->search(line, from);
    std::cmatch sm;
    bool found = std::regex_search(line.data() + from, line.data() + line.size(), sm, theirs,
                                   from > 0 ? std::regex_constants::match_prev_avail
                                            : std::regex_constants::match_default);
    auto describe = [&] {
      std::string d = "/" + pat + "/ on \"" + esc(line) + "\" from " + std::to_string(from);
      d += "\n    Regex:      ";
      if (m)
        for (std::size_t i = 0; i < Regex::kReportedGroups && i <= ours->groupCount(); ++i)
          d += m->groups[i].matched() ? "[" + std::to_string(m->groups[i].begin) + "," +
                                            std::to_string(m->groups[i].end) + ") "
                                      : "- ";
      else
        d += "no match";
      d += "\n    std::regex: ";
      if (found)
        for (std::size_t i = 0; i < Regex::kReportedGroups && i < sm.size(); ++i)
          d += sm[i].matched ? "[" + std::to_string(sm[i].first - line.data()) + "," +
                                   std::to_string(sm[i].second - line.data()) + ") "
                             : "- ";
      else
        d += "no match";
      return d;
    };
    if (m.has_value() != found)
      fail("match / no match differs: " + describe());
    if (!found)
      continue;
    for (std::size_t i = 0; i < Regex::kReportedGroups && i < sm.size(); ++i) {
      if (i > 0 && g.inRepeat[i])
        continue;
      auto const& o = m->groups[i];
      bool same =
          o.matched() == sm[i].matched &&
          (!o.matched() || (o.begin == static_cast<std::size_t>(sm[i].first - line.data()) &&
                            o.end == static_cast<std::size_t>(sm[i].second - line.data())));
      if (!same)
        fail("group " + std::to_string(i) + " differs: " + describe());
    }
  }
}

// --------------------------------------------------------------- over lines
// searchLines() over lines must find what search() finds in the same lines
// joined with '\n' into one string (patterns without ^ and $, which match
// at every line start and end over lines but only at the ends of a
// string).
static void overLines() {
  Gen g;
  g.lineBreaks = true;
  g.anchors = false;
  std::string pat = g.alternation(0).text;
  if (pat.empty())
    return;
  auto re = Regex::compile(pat);
  if (!re)
    return;
  std::vector<std::string> lines(1 + R(4));
  for (auto& l : lines)
    l = randAsciiLine(R(8));
  std::string joined;
  std::vector<std::size_t> base;
  for (auto const& l : lines) {
    if (!base.empty())
      joined += '\n';
    base.push_back(joined.size());
    joined += l;
  }
  auto point = [&](std::size_t off) {
    std::size_t i = std::upper_bound(base.begin(), base.end(), off) - base.begin() - 1;
    return BufferPoint{i, off - base[i]};
  };
  Regex::LineSource src{[&](std::size_t i) { return std::string_view(lines[i]); }, lines.size()};
  std::size_t li = R(lines.size());
  BufferPoint from{li, R(lines[li].size() + 1)};
  auto a = re->searchLines(src, from);
  auto b = re->search(joined, base[li] + from.offset);
  auto where = [&] {
    return "/" + pat + "/ on \"" + esc(joined) + "\" from " + std::to_string(from.line) + ":" +
           std::to_string(from.offset);
  };
  if (a.has_value() != b.has_value())
    fail("searchLines and search on the joined text disagree on a match: " + where());
  if (!a)
    return;
  for (std::size_t i = 0; i < Regex::kReportedGroups; ++i) {
    bool same = a->groups[i].has_value() == b->groups[i].matched() &&
                (!a->groups[i] || (a->groups[i]->first == point(b->groups[i].begin) &&
                                   a->groups[i]->second == point(b->groups[i].end)));
    if (!same)
      fail("group " + std::to_string(i) + " differs over lines: " + where());
  }
}

// ---------------------------------------------------------------- robustness
static void robustness() {
  static char const alphabet[] = "ab.()[]{}*+?|^$\\-,0123456789:dDwWsSbBxu=!<>&";
  std::string pat;
  for (std::size_t n = 1 + R(12); n; --n)
    pat += R(10) == 0 ? static_cast<char>(R(256)) : alphabet[R(sizeof alphabet - 1)];
  auto re = Regex::compile(pat);
  if (!re) {
    if (re.error().empty())
      fail("empty error message for /" + esc(pat) + "/");
    return;
  }
  for (int k = 0; k < 6; ++k) {
    std::string line = randUtf8Line(R(16));
    std::size_t from = snapToCodepoint(line, R(line.size() + 1));
    auto m = re->search(line, from);
    if (!m)
      continue;
    if (m->begin() < from || m->end() < m->begin() || m->end() > line.size())
      fail("match out of range: /" + esc(pat) + "/ on \"" + esc(line) + "\"");
    for (auto const& gspan : m->groups)
      if (gspan.matched() && (!boundary(line, gspan.begin) || !boundary(line, gspan.end) ||
                              gspan.end < gspan.begin || gspan.end > line.size()))
        fail("group off a code-point boundary: /" + esc(pat) + "/ on \"" + esc(line) + "\"");
    auto again = re->matchAt(line, m->begin());
    if (!again || again->end() != m->end())
      fail("matchAt disagrees with search: /" + esc(pat) + "/ on \"" + esc(line) + "\"");
    (void)Regex::expand("$&$1$9$`$'$$x$", line, *m);
  }
}

// ---------------------------------------------------------------------- time
static void timing(double slowMs) {
  Gen g;
  std::string pat;
  // Nullable loops allowed here: wrap random pieces in stars freely.
  for (std::size_t n = 1 + R(3); n; --n)
    pat += "(?:" + g.alternation(0).text + ")" + (R(2) ? "*" : "+");
  pat += R(2) ? "y" : "$";
  auto re = Regex::compile(pat);
  if (!re)
    return; // too large, typically
  std::string line = randAsciiLine(20000);
  auto t0 = std::chrono::steady_clock::now();
  (void)re->search(line, 0);
  double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (ms > slowMs)
    std::printf("SLOW %.0f ms: /%s/ on 20000 chars (seed %llu)\n", ms, pat.c_str(), gSeed);
}

int main(int argc, char** argv) {
  unsigned long long seed0 = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  unsigned long long iters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000;
  double slowMs = std::getenv("SLOW_MS") ? std::atof(std::getenv("SLOW_MS")) : 2000;
  std::signal(SIGABRT, onAbort);
  std::signal(SIGSEGV, onAbort);
  for (unsigned long long it = 0; it < iters; ++it) {
    gSeed = seed0 + it;
    rng.seed(gSeed);
    differential();
    overLines();
    robustness();
    if (it % 50 == 0)
      timing(slowMs);
  }
  std::printf("ok %llu iterations from seed %llu\n", iters, seed0);
}
