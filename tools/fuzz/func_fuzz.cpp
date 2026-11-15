// Property fuzzing of cx helpers: Markdown tables and
// fences, UTF-8 layout, tokenizers, prompt editing, panel layout, version
// history, plain/regex replace. Each section runs in a forked child with a
// timeout so hangs and crashes are reported and the run continues.
// usage: func_fuzz SEED ITERS [section-letters]
#include "buffer/modes/c/syntax.hpp"
#include "buffer/modes/dif/syntax.hpp"
#include "buffer/modes/jsn/syntax.hpp"
#include "buffer/modes/md/fence.hpp"
#include "buffer/modes/md/syntax.hpp"
#include "buffer/modes/md/table.hpp"
#include "buffer/modes/py/syntax.hpp"
#include "buffer/modes/sh/syntax.hpp"
#include "buffer/modes/tml/syntax.hpp"
#include "buffer/modes/yml/syntax.hpp"
#include "buffer/syntax_state.hpp"
#include "buffer/text_buffer.hpp"
#include "buffer/version_history.hpp"
#include "prompt.hpp"
#include "regex_search.hpp"
#include "replace.hpp"
#include "terminal/panel_layout.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

static std::mt19937_64 rng(1);
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }
static unsigned long long gSeed = 1, gIters = 1000;

static std::string esc(std::string_view s) {
  std::string o;
  for (unsigned char c : s) {
    if (c == '\n')
      o += "\\n";
    else if (c < 0x20 || c == 0x7f || c >= 0x80) {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    } else
      o += static_cast<char>(c);
  }
  return o;
}

[[noreturn]] static void bad(std::string const& msg) {
  std::printf("  FAIL: %s\n", msg.substr(0, 5000).c_str());
  std::fflush(stdout);
  _exit(4);
}

static int probe(char const* name, unsigned timeoutSec, std::function<void()> fn) {
  std::fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    alarm(timeoutSec);
    fn();
    std::fflush(stdout);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) {
    std::printf("[%s] %s: killed by signal %d (seed %llu)\n",
                WTERMSIG(st) == SIGALRM ? "HANG " : "CRASH", name, WTERMSIG(st), gSeed);
    return 1;
  }
  if (WEXITSTATUS(st) != 0) {
    std::printf("[EXIT ] %s: exit %d (seed %llu)\n", name, WEXITSTATUS(st), gSeed);
    return 1;
  }
  std::printf("[ok   ] %s\n", name);
  return 0;
}

static bool validUtf8(std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    if (static_cast<unsigned char>(s[i]) < 0x80) {
      ++i;
      continue;
    }
    auto len = decodeUtf8(s.data() + i, s.size() - i).second;
    if (len < 2)
      return false;
    i += len;
  }
  return true;
}

static LineStore toPtrs(std::vector<std::string> const& v) {
  LineStore out;
  for (auto const& s : v)
    out.push_back(makeLine(s));
  return out;
}

// ------------------------------------------------------------------ tables
static char const* cellBits[] = {"a",        "bb",
                                 "word",     " ",
                                 " ",        "中文",
                                 "😀",       "e\xcc\x81",
                                 "\\|",      "`a|b`",
                                 "`c d`",    "[t t](u u)",
                                 "x-y",      ":",
                                 "-",        "longerword",
                                 "\t",       "\xc2\x85",
                                 "*",        "\\",
                                 "`",        "[",
                                 "](",       "supercalifragilistic",
                                 "1.",       ">",
                                 "\xcc\x81", "\xe2\x80\x8b",
                                 "ﷺ",        "\xef\xbb\xbf",
                                 "|"};

static int gSimple = 0; // 1: plain ASCII words only; 2: plus code spans / links / wide chars
static std::string genCell(bool allowPipe) {
  std::string s;
  if (gSimple) {
    static char const* plain[] = {
        "a", "bb", "word", " ", " ", " ", "longerword", "supercalifragilistic", "x-y", "1."};
    static char const* rich[] = {
        "`c d`", "[t t](u u)", "\xe4\xb8\xad\xe6\x96\x87", "e\xcc\x81", "\\|", "*", "`"};
    for (std::size_t n = R(4) == 0 ? 0 : R(R(6) == 0 ? 40 : 6); n; --n)
      s += gSimple == 2 && R(4) == 0 ? rich[R(std::size(rich))] : plain[R(std::size(plain))];
    return s;
  }
  for (std::size_t n = R(4) == 0 ? 0 : R(R(6) == 0 ? 40 : 6); n; --n) {
    char const* b = cellBits[R(std::size(cellBits))];
    if (!allowPipe && !std::strcmp(b, "|"))
      continue;
    s += b;
  }
  return s;
}

static std::vector<std::string> genTable(bool wild) {
  std::size_t ncol = 1 + R(6);
  char const* prefixes[] = {"", "", "", "> ", "  ", ">> ", " > ", ">", "\t"};
  std::string prefix = gSimple ? "" : prefixes[R(std::size(prefixes))];
  auto row = [&](std::size_t n, bool cont, bool spacer) {
    std::string s = R(8) || gSimple ? prefix : std::string(prefixes[R(std::size(prefixes))]);
    bool lead = R(5) != 0, trail = R(5) != 0;
    if (lead || n < 2)
      s += '|';
    for (std::size_t i = 0; i < n; ++i) {
      if (i)
        s += '|';
      std::string c =
          spacer || (cont && i == 0) ? std::string(R(3), ' ') : genCell(wild && R(20) == 0);
      s += std::string(R(3), ' ') + c + std::string(R(3), ' ');
    }
    if (trail || n < 2)
      s += '|';
    return s;
  };
  std::vector<std::string> t;
  t.push_back((R(10) == 0 ? "\xef\xbb\xbf" : "") + row(ncol, false, false));
  std::string d = prefix + "|";
  char const* delims[] = {"---", ":--", "--:", ":-:", "-", " --- ", ":-", "-:", "----------"};
  for (std::size_t i = 0; i < ncol; ++i)
    d += std::string(delims[R(std::size(delims))]) + "|";
  t.push_back(d);
  for (std::size_t n = R(9); n; --n) {
    std::size_t c = R(6) == 0 ? 1 + R(8) : ncol;
    t.push_back(row(c, R(4) == 0, R(12) == 0));
  }
  if (wild)
    for (std::size_t m = R(3); m; --m) {
      auto& l = t[R(t.size())];
      if (l.empty())
        continue;
      std::size_t p = R(l.size());
      switch (R(4)) {
      case 0:
        l.erase(p, 1 + R(3));
        break;
      case 1:
        l.insert(p, 1, static_cast<char>(1 + R(255)));
        if (l[p] == '\n')
          l[p] = ' ';
        break;
      case 2:
        l[p] = "|-: \\`"[R(6)];
        break;
      case 3:
        l.insert(p, cellBits[R(std::size(cellBits))]);
        break;
      }
    }
  return t;
}

static std::string sortedPayload(std::vector<std::string> const& lines) {
  std::string s;
  for (auto const& l : lines)
    for (std::size_t i = 0; i < l.size(); ++i) {
      char c = l[i];
      if (c == ' ' || c == '\t' || c == '|' || c == ':' || c == '-' || c == '>')
        continue;
      s += c;
    }
  std::sort(s.begin(), s.end());
  return s;
}

static std::string dump(std::vector<std::string> const& lines) {
  std::string s;
  for (auto const& l : lines)
    s += "    [" + esc(l) + "]\n";
  return s;
}

static void fuzzTables() {
  for (unsigned long long it = 0; it < gIters; ++it) {
    bool wild = !gSimple && R(3) == 0;
    auto t = genTable(wild);
    std::size_t width = R(5) == 0 ? R(6) : 10 + R(120);
    std::optional<BufferPoint> cur;
    if (R(3)) {
      std::size_t l = R(t.size());
      cur = BufferPoint{
          l, clusterStart(t[l], R(t[l].size() + 1))}; // where the editor can put a cursor
    }
    auto ptrs = toPtrs(t);
    // detection consistency
    for (std::size_t i = 0; i < t.size(); ++i) {
      auto run = findPipeRun(ptrs, i);
      if (run) {
        if (run->first > i || run->last < i || run->last >= t.size())
          bad("findPipeRun range wrong\n" + dump(t));
        if (run->header && (*run->header < run->first || *run->header >= run->last))
          bad("findPipeRun header out of run / no room for delimiter\n" + dump(t));
        for (std::size_t j = run->first; j <= run->last; ++j) {
          auto r2 = findPipeRun(ptrs, j);
          if (!r2 || r2->first != run->first || r2->last != run->last || r2->header != run->header)
            bad("findPipeRun inconsistent within a run (lines " + std::to_string(i) + "," +
                std::to_string(j) + ")\n" + dump(t));
        }
      }
      auto tl = findMarkdownTable(ptrs, i);
      if (tl) {
        if (tl->first > i || tl->last < i || tl->last >= t.size())
          bad("findMarkdownTable range wrong\n" + dump(t));
        std::vector<std::string> sub(t.begin() + tl->first, t.begin() + tl->last + 1);
        if (!formatMarkdownTable(sub, std::nullopt, 80))
          bad("formatMarkdownTable rejects lines delimited by findMarkdownTable\n" + dump(sub));
      }
    }
    auto tl = findMarkdownTable(ptrs, cur ? cur->line : 0);
    if (!tl)
      continue;
    std::vector<std::string> sub(t.begin() + tl->first, t.begin() + tl->last + 1);
    std::optional<BufferPoint> c = cur;
    if (c)
      c->line -= tl->first;
    auto r = formatMarkdownTable(sub, c, width);
    if (!r)
      bad("formatMarkdownTable returned nullopt for a detected table\n" + dump(sub));
    auto ctx = [&] {
      return "width=" + std::to_string(width) +
             (c ? " cursor=" + std::to_string(c->line) + ":" + std::to_string(c->offset)
                : std::string(" no cursor")) +
             "\n  input:\n" + dump(sub) + "  output:\n" + dump(r->lines);
    };
    if (r->lines.size() < 2)
      bad("fewer than 2 output lines; " + ctx());
    if (r->cursor.has_value() != c.has_value())
      bad("cursor presence mismatch; " + ctx());
    if (r->cursor) {
      if (r->cursor->line >= r->lines.size() ||
          r->cursor->offset > r->lines[r->cursor->line].size())
        bad("cursor out of range (" + std::to_string(r->cursor->line) + ":" +
            std::to_string(r->cursor->offset) + "); " + ctx());
      auto const& l = r->lines[r->cursor->line];
      if (validUtf8(l) && snapToCodepoint(l, r->cursor->offset) != r->cursor->offset)
        bad("cursor inside a code point; " + ctx());
    }
    bool allValid = true;
    for (auto const& l : sub)
      allValid = allValid && validUtf8(l);
    for (auto const& l : r->lines) {
      if (allValid && !validUtf8(l))
        bad("output is not valid UTF-8; " + ctx());
      if (l.find('\n') != std::string::npos)
        bad("newline inside an output line; " + ctx());
    }
    if (sortedPayload(sub) != sortedPayload(r->lines))
      bad("cell text changed (payload bytes differ); " + ctx());
    std::size_t w0 = visualWidth(r->lines[0]);
    // The BOM stays on the first line only; it is escaped-width, so compare from line 1.
    for (std::size_t i = 2; i < r->lines.size(); ++i)
      if (visualWidth(r->lines[i]) != visualWidth(r->lines[1]))
        bad("rows are not equally wide (line " + std::to_string(i) + "); " + ctx());
    (void)w0;
    // idempotence
    auto r2 = formatMarkdownTable(r->lines, r->cursor, width);
    if (!r2)
      bad("formatted table is no longer a table; " + ctx());
    if (r2->lines != r->lines)
      bad("not idempotent; " + ctx() + "  second pass:\n" + dump(r2->lines));
    if (r2->cursor != r->cursor)
      bad("cursor moves on second pass (" + std::to_string(r->cursor->line) + ":" +
          std::to_string(r->cursor->offset) + " -> " + std::to_string(r2->cursor->line) + ":" +
          std::to_string(r2->cursor->offset) + "); " + ctx());
    // still detected as one table
    auto p2 = toPtrs(r->lines);
    auto tl2 = findMarkdownTable(p2, 0);
    if (!tl2 || tl2->first != 0 || tl2->last + 1 != r->lines.size())
      bad("output not detected as a single table; " + ctx());
  }
}

static void tablePerf() {
  for (std::size_t rows : {2000u, 20000u, 100000u}) {
    std::vector<std::string> t{"| a | b | c |", "|---|---|---|"};
    for (std::size_t i = 0; i < rows; ++i)
      t.push_back(i % 3 ? "| x | some words to wrap here and there and more | z |"
                        : "|  | cont text | |");
    auto t0 = std::chrono::steady_clock::now();
    auto r = formatMarkdownTable(t, BufferPoint{rows / 2, 10}, 40);
    auto ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto ptrs = toPtrs(t);
    auto t1 = std::chrono::steady_clock::now();
    auto run = findPipeRun(ptrs, rows / 2);
    auto fenced = markdownFencedLines(ptrs);
    auto ms2 =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    std::printf("        %zu rows: format %.0f ms (%zu lines out), findPipeRun+fences %.1f ms\n",
                rows, ms, r ? r->lines.size() : 0, ms2);
    (void)run;
  }
  // One enormous row / cell
  std::vector<std::string> t{"| a | b |", "|---|---|", "| x | " + std::string(2000000, 'w') + " |"};
  auto t0 = std::chrono::steady_clock::now();
  auto r = formatMarkdownTable(t, BufferPoint{2, 20}, 80);
  std::string many = "| x | ";
  for (int i = 0; i < 300000; ++i)
    many += "w ";
  many += "|";
  std::vector<std::string> t2{"| a | b |", "|---|---|", many};
  auto r2 = formatMarkdownTable(t2, BufferPoint{2, 20}, 80);
  std::string cols = "|", del = "|";
  for (int i = 0; i < 20000; ++i)
    cols += "c|", del += "-|";
  std::vector<std::string> t3{cols, del, cols};
  auto r3 = formatMarkdownTable(t3, BufferPoint{2, 20}, 80);
  auto ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("        2MB cell + 300k words + 20k columns: %.0f ms (%zu/%zu/%zu lines)\n", ms,
              r ? r->lines.size() : 0, r2 ? r2->lines.size() : 0, r3 ? r3->lines.size() : 0);
}

// ------------------------------------------------------------------ fences
static void fuzzFences() {
  char const* bits[] = {"```", "~~~",  "````", "`",     "~",  " ", "   ", "    ",
                        "\t",  "text", "```c", "``` `", "> ", "",  "|",   "~~~~~"};
  for (unsigned long long it = 0; it < gIters; ++it) {
    std::vector<std::string> t;
    for (std::size_t n = R(12); n; --n) {
      std::string l;
      for (std::size_t k = R(4); k; --k)
        l += bits[R(std::size(bits))];
      t.push_back(l);
    }
    auto p = toPtrs(t);
    auto f = markdownFencedLines(p);
    if (f.size() != t.size())
      bad("markdownFencedLines size mismatch\n" + dump(t));
    // A fenced region is a run that starts with a fence-looking line.
    for (std::size_t i = 0; i < t.size(); ++i)
      if (f[i] && (i == 0 || !f[i - 1])) {
        auto const& l = t[i];
        auto a = l.find_first_not_of(" \t");
        if (a == std::string::npos || (l.compare(a, 3, "```") && l.compare(a, 3, "~~~")))
          bad("fenced run starts on a non-fence line " + std::to_string(i) + "\n" + dump(t));
      }
  }
}

// ------------------------------------------------------------------- UTF-8
static std::string randU8(std::size_t n) {
  static char const* bits[] = {"a",
                               "b",
                               " ",
                               "\t",
                               "é",
                               "中",
                               "😀",
                               "e",
                               "\xcc\x81",
                               "\xcc\x82",
                               "\xe2\x80\x8b",
                               "\xef\xbb\xbf",
                               "\xc2\x85",
                               "\xc2\x9b",
                               "\x1b",
                               "\x1b[",
                               "m",
                               "\x01",
                               "\x7f",
                               "\xe2\x80\xae",
                               "\xf0\x9f\x8f\xbd",
                               "\xe2\x80\x8d",
                               "\xe1\x84\x80",
                               "\xe1\x85\xa1",
                               "\xf4\x8f\xbf\xbf",
                               "\xed\xa0\x80",
                               "\xc0\xaf",
                               "\xf4\x90\x80\x80",
                               "Ａ",
                               "\xff",
                               "\xc3",
                               "\xe2\x82",
                               "\xf0\x9f\x98",
                               "\x80",
                               "\xcd\x8f",
                               "\xd6\x91",
                               "\xe0\xa4\xbe",
                               "\xf3\xa0\x84\x80"};
  std::string s;
  for (; n; --n)
    s += R(40) == 0 ? std::string(1, static_cast<char>(R(256)))
                    : std::string(bits[R(std::size(bits))]);
  return s;
}

static void fuzzUtf8() {
  // exhaustive: every code point
  for (uint32_t cp = 0; cp <= 0x110010; ++cp) {
    auto [n, enc] = encodeUtf8(cp);
    std::string s(reinterpret_cast<char const*>(enc.data()), n);
    if (s.empty())
      continue;
    auto ci = charInfoAt(s.data(), s.size());
    if (ci.len == 0 || ci.len > s.size())
      bad("charInfoAt len out of range for U+" + std::to_string(cp));
    auto e = escapeNonPrintable(s);
    if (!validUtf8(e))
      bad("escapeNonPrintable yields invalid UTF-8 for U+" + std::to_string(cp));
    for (std::size_t i = 0; i < e.size();) {
      auto di = displayCharInfoAt(e.data() + i, e.size() - i);
      auto c = static_cast<unsigned char>(e[i]);
      if (di.width != 0 && (c < 0x20 || c == 0x7f))
        bad("escapeNonPrintable leaves a control byte for U+" + std::to_string(cp));
      i += di.len;
    }
    if (cp >= 0x80 && cp < 0xD800 && isEscapedCodepoint(cp)) {
      auto ce = codepointEscape(cp);
      if (visualWidth(ce) != ci.width)
        bad("escape width mismatch for U+" + std::to_string(cp) + ": drawn " +
            std::to_string(visualWidth(ce)) + " measured " + std::to_string(ci.width));
    }
  }
  for (unsigned long long it = 0; it < gIters; ++it) {
    std::string s = randU8(R(30));
    bool hasEsc = s.find('\x1b') != std::string::npos;
    // cluster walk
    std::vector<std::size_t> starts;
    std::size_t total = 0;
    for (std::size_t i = 0; i < s.size();) {
      auto ci = charInfoAt(s.data() + i, s.size() - i);
      if (ci.len == 0 || i + ci.len > s.size())
        bad("charInfoAt len out of range at " + std::to_string(i) + " in [" + esc(s) + "]");
      starts.push_back(i);
      total += ci.width;
      i += ci.len;
    }
    if (!hasEsc && visualWidth(s) != total)
      bad("visualWidth " + std::to_string(visualWidth(s)) + " != sum of clusters " +
          std::to_string(total) + " [" + esc(s) + "]");
    if (visualColRange(s, 0, s.size()) != total)
      bad("visualColRange != sum of clusters [" + esc(s) + "]");
    for (std::size_t off = 0; off <= s.size(); ++off) {
      auto cs = clusterStart(s, off);
      if (cs > off)
        bad("clusterStart > off [" + esc(s) + "] off " + std::to_string(off));
      if (off < s.size()) {
        auto it2 = std::upper_bound(starts.begin(), starts.end(), off);
        std::size_t want = *(it2 - 1);
        if (cs != want)
          bad("clusterStart(" + std::to_string(off) + ")=" + std::to_string(cs) +
              " but the forward walk says " + std::to_string(want) + " [" + esc(s) + "]");
      } else if (cs != s.size())
        bad("clusterStart(size) != size");
      auto sn = snapToCodepoint(s, off);
      if (sn > off || off - sn > 3)
        bad("snapToCodepoint moved too far [" + esc(s) + "]");
    }
    for (std::size_t i = 1; i < starts.size(); ++i)
      if (prevClusterStart(s, starts[i]) != starts[i - 1] ||
          nextClusterStart(s, starts[i - 1]) != starts[i])
        bad("prev/nextClusterStart disagree with the walk [" + esc(s) + "]");
    // wrapping
    std::size_t cols = R(6) == 0 ? R(3) : 1 + R(30);
    auto rb = computeRowBounds(s, cols);
    if (rb.empty() || rb[0] != 0)
      bad("computeRowBounds does not start at 0 [" + esc(s) + "] cols " + std::to_string(cols));
    for (std::size_t i = 0; i < rb.size(); ++i) {
      if (i && rb[i] <= rb[i - 1])
        bad("computeRowBounds not strictly increasing [" + esc(s) + "] cols " +
            std::to_string(cols));
      if (rb[i] > s.size() ||
          (rb[i] < s.size() && !std::binary_search(starts.begin(), starts.end(), rb[i])))
        bad("row bound " + std::to_string(rb[i]) + " not on a cluster boundary [" + esc(s) +
            "] cols " + std::to_string(cols));
      if (cols > 0) {
        std::size_t end = i + 1 < rb.size() ? rb[i + 1] : s.size();
        std::size_t w = visualColRange(s, rb[i], end);
        bool single = std::upper_bound(starts.begin(), starts.end(), rb[i]) ==
                      std::lower_bound(starts.begin(), starts.end(), end);
        if (w > cols && !single)
          bad("visual row " + std::to_string(i) + " is " + std::to_string(w) + " wide > cols " +
              std::to_string(cols) + " [" + esc(s) + "]");
      }
    }
    if (cols > 0) {
      if (lastVisualRowStart(s, cols) != rb.back())
        bad("lastVisualRowStart " + std::to_string(lastVisualRowStart(s, cols)) +
            " != last row bound " + std::to_string(rb.back()) + " [" + esc(s) + "] cols " +
            std::to_string(cols));
      for (std::size_t off = 0; off <= s.size(); ++off) {
        auto a = findVisualRow(s, off, cols);
        auto b = findVisualRowCached(rb, off, s.size());
        if (a.prevRowStart != b.prevRowStart || a.curRowStart != b.curRowStart ||
            a.nextRowStart != b.nextRowStart)
          bad("findVisualRow {" + std::to_string(a.prevRowStart) + "," +
              std::to_string(a.curRowStart) + "," + std::to_string(a.nextRowStart) +
              "} != cached {" + std::to_string(b.prevRowStart) + "," +
              std::to_string(b.curRowStart) + "," + std::to_string(b.nextRowStart) + "} off " +
              std::to_string(off) + " cols " + std::to_string(cols) + " [" + esc(s) + "]");
        if (a.curRowStart > off && off < s.size() &&
            std::binary_search(starts.begin(), starts.end(), off))
          bad("findVisualRow curRowStart > off [" + esc(s) + "]");
      }
    }
    std::size_t from = starts.empty() ? 0 : starts[R(starts.size())];
    std::size_t goal = R(40);
    auto o = offsetForVisualCol(s, from, s.size(), goal);
    if (o < from || o > s.size() ||
        (o < s.size() && !std::binary_search(starts.begin(), starts.end(), o)))
      bad("offsetForVisualCol result " + std::to_string(o) + " not a cluster boundary in range [" +
          esc(s) + "]");
    if (visualColRange(s, from, o) > goal)
      bad("offsetForVisualCol overshoots the goal [" + esc(s) + "] from " + std::to_string(from) +
          " goal " + std::to_string(goal));
    std::size_t mw = R(20);
    auto e = escapeNonPrintable(s);
    auto tr = truncateVisualFromFront(e, mw);
    if (visualWidth(tr) > mw)
      bad("truncateVisualFromFront wider than max " + std::to_string(mw) + ": [" + esc(tr) +
          "] from [" + esc(e) + "]");
    if (!validUtf8(e) || !validUtf8(tr))
      bad("escape/truncate produced invalid UTF-8 from [" + esc(s) + "]");
  }
}

// --------------------------------------------------------------- tokenizers
// Every tokenizer behind one signature: the mode's state lives in the
// SyntaxState variant, as in the buffer's syntax cache.
using TokFn = void (*)(std::string const&, SyntaxState&, std::vector<SyntaxToken>&);
template <class S, void (*Fn)(std::string const&, S&, std::vector<SyntaxToken>&)>
static void withState(std::string const& line, SyntaxState& st, std::vector<SyntaxToken>& out) {
  Fn(line, syntaxStateFor<S>(st), out);
}
template <void (*Fn)(std::string const&, std::vector<SyntaxToken>&)>
static void stateless(std::string const& line, SyntaxState&, std::vector<SyntaxToken>& out) {
  Fn(line, out);
}
static void fuzzTokenizers() {
  struct M {
    char const* name;
    TokFn fn;
    std::vector<char const*> bits;
  };
  std::vector<char const*> common = {" ",  "  ", "\t", "a", "x1", "_",  "0",    "1.5e3",    "0x1F",
                                     "\"", "'",  "\\", "#", "/",  "*",  "(",    ")",        "[",
                                     "]",  "{",  "}",  "<", ">",  "=",  ":",    ",",        ".",
                                     "-",  "+",  "|",  "&", "$",  "`",  "!",    "~",        "@",
                                     "%",  "^",  "?",  ";", "é",  "中", "\xff", "\xcc\x81", "\r"};
  std::vector<M> modes = {
      {"c",
       withState<CSyntaxState, tokenizeLine>,
       {"/*",       "*/", "//",     "R\"",  "R\"d(", ")d\"",      ")\"",      "#include",
        "#define",  "\\", "u8R\"(", "L'",   "int",   "const",     "co_await", "'\\''",
        "\"\\\"\"", "<<", "0b1'0",  "?\?/", "#if 0", "template<", "::",       "->"}},
      {"md",
       withState<MdSyntaxState, tokenizeMarkdownLine>,
       {"# ",      "## ", "**", "__",  "*",   "_",  "`",     "```",  "~~~",
        "[",       "](",  ")",  "![",  "> ",  "- ", "1. ",   "<",    ">",
        "http://", "\\",  "|",  "---", "***", "  ", "[x]: ", "<!--", "-->"}},
      {"jsn",
       stateless<tokenizeJsonLine>,
       {"{", "}", "[", "]", "\"", "\\\"", "\\u12", ":", ",", "true", "false", "null", "-1", "1e",
        "//", "/*"}},
      {"dif",
       stateless<tokenizeDiffLine>,
       {"+", "-", "@@", "@@ -1,2 +3,4 @@", "diff --git", "index ", "--- ", "+++ ", " ",
        "\\ No newline", "<<<<<<<", "=======", ">>>>>>>"}},
      {"sh",
       withState<ShSyntaxState, tokenizeShellLine>,
       {"<<", "<<-",   "<<<",   "EOF", "'EOF'", "\"EOF\"", "$",   "${", "}",
        "$(", ")",     "`",     "#",   "'",     "\"",      "\\",  "if", "then",
        "fi", "<<EOF", "\tEOF", "<< ", "<<''",  "<<\\E",   "$((", "))", ";;"}},
      {"yml",
       withState<YmlSyntaxState, tokenizeYamlLine>,
       {"key:", ": ", "- ",    "|",    ">",    "|-",  ">+", "|2",  "#", "\"",
        "'",    "&a", "*a",    "!!t",  "---",  "...", "{",  "}",   "[", "]",
        ", ",   "? ", "%YAML", "true", "null", "~",   "  ", "    "}},
      {"tml",
       withState<TmlSyntaxState, tokenizeTomlLine>,
       {"[", "]", "[[", "]]", "=",    "\"\"\"",     "'''", "\"",  "'",   "\\",       "#",
        "{", "}", ",",  ".",  "true", "1979-05-27", "0x",  "inf", "nan", "\"\"\"\"", "''''"}},
      {"py",
       withState<PySyntaxState, tokenizePythonLine>,
       {"\"\"\"", "'''",    "r'",     "f\"", "rb'''", "\"", "'", "\\", "#",    "@",
        "def ",   "class ", "lambda", ":",   "0x_1",  "1j", "{", "}",  "\\\"", "u'"}},
  };
  for (auto& m : modes) {
    for (unsigned long long it = 0; it < gIters; ++it) {
      SyntaxState st;
      for (std::size_t ln = 1 + R(12); ln; --ln) {
        std::string line;
        for (std::size_t n = R(R(50) == 0 ? 400 : 14); n; --n) {
          auto const& src = R(3) ? m.bits : common;
          line += src[R(src.size())];
        }
        SyntaxState before = st;
        std::vector<SyntaxToken> out;
        m.fn(line, st, out);
        if (out.size() != line.size())
          bad(std::string(m.name) + ": token count " + std::to_string(out.size()) +
              " != line size " + std::to_string(line.size()) + " [" + esc(line) + "]");
        // Reusing a dirty output vector must give the same answer.
        std::vector<SyntaxToken> out2(R(5), SyntaxToken::Comment);
        SyntaxState st2 = before;
        m.fn(line, st2, out2);
        if (out2 != out)
          bad(std::string(m.name) + ": tokens depend on the output vector's previous contents [" +
              esc(line) + "]");
      }
    }
  }
  // long-line timing (per mode, pathological repeats)
  for (auto& m : modes) {
    for (auto bit : m.bits) {
      std::string line;
      while (line.size() < 300000)
        line += bit;
      SyntaxState st;
      std::vector<SyntaxToken> out;
      auto t0 = std::chrono::steady_clock::now();
      m.fn(line, st, out);
      m.fn(line, st, out);
      auto ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (ms > 1500)
        std::printf("        SLOW %s tokenizer: %.0f ms for 2x300KB of [%s]\n", m.name, ms,
                    esc(bit).c_str());
    }
  }
}

// ------------------------------------------------------------------ prompt
static void fuzzPrompt() {
  for (unsigned long long it = 0; it < gIters; ++it) {
    Prompt p;
    bool valid = true;
    for (std::size_t n = 60; n; --n) {
      std::string desc;
      switch (R(18)) {
      case 0:
      case 1:
      case 2: {
        auto t = randU8(R(4));
        valid = valid && validUtf8(t);
        p.insert(t);
        break;
      }
      case 3:
        p.deleteBefore();
        break;
      case 4:
        p.deleteAt();
        break;
      case 5:
        p.deleteChunkBefore();
        break;
      case 6:
        p.deleteChunkAt();
        break;
      case 7:
        p.moveLeft();
        break;
      case 8:
        p.moveRight();
        break;
      case 9:
        p.moveWordLeft();
        break;
      case 10:
        p.moveWordRight();
        break;
      case 11:
        p.moveHome();
        break;
      case 12:
        p.moveEnd();
        break;
      case 13:
        p.toggleMark();
        break;
      case 14:
        p.deleteRegion();
        break;
      case 15:
        p.input = randU8(R(6));
        valid = validUtf8(p.input);
        p.inputReplaced();
        break;
      case 16:
        p.input.resize(R(p.input.size() + 1));
        valid = false;
        break; // raw truncation: boundaries no longer tracked
      case 17:
        p.insert("中");
        break;
      }
      if (p.cursorPos() > p.input.size())
        bad("prompt cursor past the end");
      if (auto r = p.region(); r && (r->first > r->second || r->second > p.input.size()))
        bad("prompt region out of range");
      if (valid && !validUtf8(p.input))
        bad("prompt input became invalid UTF-8: [" + esc(p.input) + "]");
      if (valid && snapToCodepoint(p.input, p.cursorPos()) != p.cursorPos())
        bad("prompt cursor inside a code point: [" + esc(p.input) + "] at " +
            std::to_string(p.cursorPos()));
    }
  }
}

// ------------------------------------------------------------ panel layout
static void fuzzPanels() {
  for (unsigned long long it = 0; it < gIters / 10 + 1; ++it) {
    TextBuffer a("a"), b("b");
    std::size_t tr = 1 + R(60), tc = 1 + R(200);
    PanelLayout lay(a, tr, tc);
    for (std::size_t n = 80; n; --n) {
      switch (R(8)) {
      case 0:
      case 1:
        lay.splitFocused(R(2) ? SplitDir::Vertical : SplitDir::Horizontal, R(2) ? a : b);
        break;
      case 2:
        lay.closeFocused();
        break;
      case 3:
        lay.focusPanel(static_cast<int>(R(12)));
        break;
      case 4:
        lay.focusMRU();
        break;
      case 5:
        tr = R(8) == 0 ? R(4) : 1 + R(80);
        tc = R(8) == 0 ? R(4) : 1 + R(250);
        lay.resize(tr, tc);
        break;
      case 6:
        lay.focused().view().insertText("x\ny");
        break;
      case 7:
        (void)lay.viewCountFor(a);
        break;
      }
      std::vector<PanelRect> rects;
      bool focusedSeen = false;
      int fid = lay.focused().id();
      lay.forEachPanel([&](Panel& p) {
        rects.push_back(p.rect());
        focusedSeen = focusedSeen || p.id() == fid;
        if (lay.byId(p.id()) != &p)
          bad("byId does not return the panel");
      });
      if (rects.size() != lay.count() || rects.empty())
        bad("panel count mismatch");
      if (!focusedSeen)
        bad("focused panel is not in the tree");
      for (std::size_t i = 0; i < rects.size(); ++i) {
        auto const& x = rects[i];
        if (x.rows > (1u << 20) || x.cols > (1u << 20) || x.row > (1u << 20) || x.col > (1u << 20))
          bad("panel rect underflow: row " + std::to_string(x.row) + " col " +
              std::to_string(x.col) + " rows " + std::to_string(x.rows) + " cols " +
              std::to_string(x.cols) + " term " + std::to_string(tr) + "x" + std::to_string(tc));
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
          auto const& y = rects[j];
          bool overlap = x.row < y.row + y.rows && y.row < x.row + x.rows &&
                         x.col < y.col + y.cols && y.col < x.col + x.cols;
          if (overlap && x.rows && x.cols && y.rows && y.cols && tr > 6 && tc > 12)
            bad("panels overlap on a " + std::to_string(tr) + "x" + std::to_string(tc) +
                " terminal with " + std::to_string(rects.size()) + " panels");
        }
      }
      // every panel must render into its rect without tripping checks
      lay.forEachPanel([&](Panel& p) {
        std::string out;
        if (p.rect().rows && p.rect().cols) {
          p.view().ensureVisible(RowIdx{p.rect().rows}, ColIdx{p.rect().cols}, p.view().wrap());
          p.view().render(out, RowIdx{p.rect().rows}, ColIdx{p.rect().cols});
        }
      });
    }
  }
}

// --------------------------------------------------------- version history
static std::string joinLines(LineStore const& v) {
  std::string s;
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i)
      s += '\n';
    s += *v[i];
  }
  return s;
}

static void fuzzHistory() {
  for (unsigned long long it = 0; it < gIters / 20 + 1; ++it) {
    VersionHistory h;
    LineStore lines(4); // tiny chunks: every edit splits or merges somewhere
    for (std::size_t n = 1 + R(30); n; --n)
      lines.push_back(makeLine(std::to_string(R(1000))));
    std::vector<std::string> states;
    auto mk = [](std::string s) { return std::make_shared<std::string const>(std::move(s)); };
    h.capture(lines, {0, 0});
    states.push_back(joinLines(lines));
    int stamp = 0;
    for (std::size_t n = 50 + R(600); n; --n) {
      int k = static_cast<int>(R(100));
      if (k < 60) { // single-line edit
        std::size_t l = R(lines.size());
        lines.replace(l, mk(*lines[l] + "." + std::to_string(++stamp)));
        h.capture(lines, {l, 0});
        states.push_back(joinLines(lines));
      } else if (k < 72) { // insert lines
        std::size_t l = R(lines.size() + 1), cnt = 1 + R(3);
        for (std::size_t i = 0; i < cnt; ++i)
          lines.insert(l, mk("n" + std::to_string(++stamp)));
        h.capture(lines, {l, 0});
        states.push_back(joinLines(lines));
      } else if (k < 82) { // delete lines
        if (lines.size() > 1) {
          std::size_t l = R(lines.size() - 1),
                      cnt = 1 + R(std::min<std::size_t>(3, lines.size() - 1 - l));
          lines.erase(l, l + cnt);
          h.capture(lines, {l, 0});
          states.push_back(joinLines(lines));
        }
      } else if (k < 88) { // unknown extent
        for (std::size_t m = 1 + R(3); m; --m) {
          std::size_t l = R(lines.size());
          lines.replace(l, mk("u" + std::to_string(++stamp)));
        }
        h.capture(lines, {0, 0});
        states.push_back(joinLines(lines));
      } else if (k < 93) { // browse a random version
        std::size_t i = R(h.size());
        (void)h.linesAt(i);
      } else if (k < 96) { // commit an older version (as exitHistoryModeCommit does)
        std::size_t i = R(h.size());
        lines = h.linesAt(i);
        h.truncateAfter(i);
        states.push_back(joinLines(lines));
      } else { // verify below
      }
      if (h.size() == 0 || h.size() > VersionHistory::kMaxVersions + 1)
        bad("history size " + std::to_string(h.size()) + " out of bounds");
      if (k >= 96 || n == 1) {
        std::size_t si = 0;
        for (std::size_t i = 0; i < h.size(); ++i) {
          auto t = joinLines(h.linesAt(i));
          std::size_t j = si;
          while (j < states.size() && states[j] != t)
            ++j;
          if (j == states.size()) {
            if (std::find(states.begin(), states.end(), t) == states.end())
              bad("history version " + std::to_string(i) + "/" + std::to_string(h.size()) +
                  " is a text that never existed: [" + esc(t).substr(0, 600) + "]");
          } else
            si = j;
        }
        if (joinLines(h.linesAt(h.size() - 1)) != joinLines(lines))
          bad("newest version != current lines");
      }
    }
  }
}

// ---------------------------------------------------------- replace / regex
static std::unique_ptr<TextBuffer> mkbuf(std::string const& content) {
  auto b = std::make_unique<TextBuffer>("t");
  b->insertText(content, BufferPoint{0, 0});
  return b;
}
static std::string bufText(Buffer const& b) {
  std::string s;
  for (std::size_t i = 0; i < b.lineCount(); ++i) {
    if (i)
      s += '\n';
    s += b.lineContent(i);
  }
  return s;
}

static void fuzzReplace() {
  char const* alpha[] = {"a", "b", "ab", " ", "\n", "é", "中", "aa", "x", "."};
  auto rnd = [&](std::size_t n, bool nl) {
    std::string s;
    for (; n; --n) {
      char const* p = alpha[R(std::size(alpha))];
      if (!nl && *p == '\n')
        continue;
      s += p;
    }
    return s;
  };
  for (unsigned long long it = 0; it < gIters; ++it) {
    std::string content = rnd(R(60), true), pat = rnd(1 + R(3), false), rep = rnd(R(4), true);
    if (pat.empty())
      continue;
    auto b = mkbuf(content);
    auto n = replaceAllForward(*b, {0, 0}, pat, rep);
    std::string model;
    std::size_t cnt = 0;
    for (std::size_t i = 0; i < content.size();) {
      if (content.compare(i, pat.size(), pat) == 0) {
        model += rep;
        i += pat.size();
        ++cnt;
      } else
        model += content[i++];
    }
    if (bufText(*b) != model || n != cnt)
      bad("replaceAllForward mismatch: content [" + esc(content) + "] pat [" + esc(pat) +
          "] rep [" + esc(rep) + "]\n    got    [" + esc(bufText(*b)) + "] (" + std::to_string(n) +
          ")\n    expect [" + esc(model) + "] (" + std::to_string(cnt) + ")");
  }
  // regex: termination + sanity against std::regex_replace on single lines
  char const* pats[] = {"a",    "a+",  "a*",    "^",    "$",    "\\b", "(a)(b)?", "[ab]",
                        ".",    "x*",  "(?=a)", "^a",   "b$",   "a|",  "\\s",     "(é|中)",
                        "\\w+", "\\W", "",      "(a*)", "a{2}", "\\B", ".?"};
  char const* reps[] = {"", "X", "$1", "[$&]", "$$", "\n", "$2$1", "aa", "中"};
  for (unsigned long long it = 0; it < gIters / 4 + 1; ++it) {
    std::string content = rnd(R(40), true);
    std::string pat = pats[R(std::size(pats))], rep = reps[R(std::size(reps))];
    std::string err;
    auto re = compileRegex(pat, err);
    if (!re)
      continue;
    auto b = mkbuf(content);
    auto n = replaceAllRegexForward(*b, {0, 0}, *re, rep);
    auto got = bufText(*b);
    bool ascii = std::all_of(content.begin(), content.end(),
                             [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (ascii && rep.find('\n') == std::string::npos) {
      // Per-line std::regex_replace is the reference for a single pass.
      std::string model;
      std::size_t ls = 0;
      for (;;) {
        auto le = content.find('\n', ls);
        std::string line =
            content.substr(ls, le == std::string::npos ? std::string::npos : le - ls);
        model += std::regex_replace(line, *re, rep);
        if (le == std::string::npos)
          break;
        model += '\n';
        ls = le + 1;
      }
      if (got != model)
        bad("replaceAllRegexForward differs from per-line std::regex_replace: content [" +
            esc(content) + "] /" + pat + "/ -> [" + esc(rep) + "]\n    got    [" + esc(got) +
            "] (" + std::to_string(n) + ")\n    expect [" + esc(model) + "]");
    }
    // find helpers stay in range
    for (std::size_t li = 0; li < b->lineCount(); ++li) {
      auto const& line = b->lineContent(li);
      std::size_t from = R(line.size() + 1);
      if (auto m = findFirstRegex(line, from, *re);
          m && (m->first < from || m->second < m->first || m->second > line.size()))
        bad("findFirstRegex out of range");
      std::size_t lim = R(line.size() + 1);
      if (auto m = findLastRegex(line, 0, lim, *re);
          m && (m->second < m->first || m->second > line.size()))
        bad("findLastRegex out of range");
    }
  }
}

int main(int argc, char** argv) {
  gSeed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  gIters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1000;
  std::string which = argc > 3 ? argv[3] : "tfuspLhr";
  rng.seed(gSeed);
  if (char const* e = std::getenv("FF_SIMPLE"))
    gSimple = std::atoi(e);
  int fails = 0;
  auto run = [&](char c, char const* name, unsigned to, void (*fn)()) {
    if (which.find(c) != std::string::npos)
      fails += probe(name, to, fn);
  };
  run('t', "markdown tables: detect/format properties", 600, fuzzTables);
  run('T', "markdown tables: scaling", 120, tablePerf);
  run('f', "markdown fences", 300, fuzzFences);
  run('u', "utf8 layout properties", 900, fuzzUtf8);
  run('s', "tokenizers (all modes)", 900, fuzzTokenizers);
  run('p', "prompt editing", 300, fuzzPrompt);
  run('L', "panel layout", 600, fuzzPanels);
  run('h', "version history vs. mirror", 900, fuzzHistory);
  run('r', "plain/regex replace-all vs. model", 600, fuzzReplace);
  std::printf("func_fuzz seed %llu done, %d failing section(s)\n", gSeed, fails);
  return fails ? 1 : 0;
}
