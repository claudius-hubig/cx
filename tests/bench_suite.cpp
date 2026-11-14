// Editor benchmark suite: per-operation latency and peak memory across file
// shapes and cursor counts, as a baseline for storage and history changes.
//
// Usage:
//   ./build/bench_suite [--quick] [--profile NAME,...] [--filter PREFIX,...]
//                       [--cursors N,...] [--csv FILE] [--compare FILE]
//                       [--mem-limit MB] [--list]
//
//   --quick        Only the profiles small, cpp and medium (~10 s; the
//                  full run takes ~2 min).
//   --profile      Run only the named profiles (see --list).
//   --filter       Run only operations whose name starts with one of the
//                  prefixes, e.g. `edit.,multi.type` (groups whose ops are
//                  all filtered out are not even set up).
//   --cursors      Cursor counts for the multi.* operations (1..20,
//                  default 1,5,20).
//   --csv          Write every result row to FILE.
//   --compare      Read an earlier --csv FILE and print each median next to
//                  its baseline; `+` marks >10% slower, `-` >10% faster
//                  (ignoring differences under 1 us / 1 MB).
//   --mem-limit    Address-space limit per profile in MB (default 2048,
//                  0 = none). Exceeding it is reported as bad_alloc for the
//                  operation that hit it, and the rest of its group is
//                  skipped, instead of the machine swapping.
//
// Every operation goes through the same path as the editor: BufferView
// methods (which record history versions), or the free search/replace
// helpers Screen calls. Each profile runs in a forked child, so the
// address-space limit applies per profile and one profile's heap does not
// distort the next one's memory figures. This uses POSIX/Linux interfaces
// (fork, setrlimit, /proc), like bench_history does.
//
// Timings are wall-clock microseconds per call. Rows are
// `op  xC  n=N  median  p95  max`, where C is the number of active cursors.
// Memory rows are in MB: `<group>.peak-growth` is the group's peak RSS
// (VmHWM, reset via /proc/self/clear_refs) above its starting RSS (after
// malloc_trim), and `history.*.rss-growth` the RSS a keystroke burst left
// behind.

#include "test_helpers.hpp"

#include "buffer/text_buffer.hpp"
#include "regex_search.hpp"
#include "replace.hpp"
#include "search.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

double sinceUs(Clock::time_point t0) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
}

// Defeats dead-code elimination of read-only operations.
std::size_t volatile g_sink = 0;

std::vector<std::string> splitList(std::string_view s) {
  std::vector<std::string> out;
  std::string item;
  std::istringstream in{std::string(s)};
  while (std::getline(in, item, ','))
    if (!item.empty())
      out.push_back(item);
  return out;
}

// ---------------------------------------------------------------------------
// Memory (Linux /proc)
// ---------------------------------------------------------------------------

// A "VmRSS:"-style field of /proc/self/status in MB, or 0 if unavailable.
double procStatusMb(std::string_view key) {
  std::ifstream in("/proc/self/status");
  for (std::string line; std::getline(in, line);)
    if (line.starts_with(key))
      return std::stod(line.substr(key.size())) / 1024.0;
  return 0;
}
double rssMb() { return procStatusMb("VmRSS:"); }
double peakRssMb() { return procStatusMb("VmHWM:"); }
// Resets VmHWM to the current RSS (Linux >= 4.0); harmless if refused.
void resetPeakRss() { std::ofstream("/proc/self/clear_refs") << "5"; }

// ---------------------------------------------------------------------------
// Options and baseline
// ---------------------------------------------------------------------------

struct Options {
  bool quick = false;
  std::vector<std::string> profiles;
  std::vector<std::string> filters;
  std::vector<std::size_t> cursors{1, 5, 20};
  std::string csv;
  std::string compare;
  std::size_t memLimitMb = 2048;
};
Options g_opt;

bool wanted(std::string_view op) {
  return g_opt.filters.empty() ||
         std::ranges::any_of(g_opt.filters, [&](auto const& f) { return op.starts_with(f); });
}
// True if some operation under @p prefix can pass the filter.
bool groupWanted(std::string_view prefix) {
  return g_opt.filters.empty() || std::ranges::any_of(g_opt.filters, [&](auto const& f) {
           return prefix.starts_with(f) || std::string_view(f).starts_with(prefix);
         });
}

struct Stats {
  std::size_t n = 0;
  double median = 0, p95 = 0, max = 0, mean = 0;
};

Stats statsOf(std::vector<double> v) {
  std::ranges::sort(v);
  auto n = v.size();
  return {n, v[n / 2], v[std::min(n - 1, n * 95 / 100)], v.back(),
          std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(n)};
}

struct Key {
  std::string profile, op;
  std::size_t cursors;
  auto operator<=>(Key const&) const = default;
};
std::map<Key, double> g_baseline; // median per row

constexpr char const* kCsvHeader = "profile,op,cursors,n,median,p95,max,mean,unit";

void loadBaseline(std::string const& path) {
  std::ifstream in(path);
  if (!in) {
    std::fprintf(stderr, "cannot read %s\n", path.c_str());
    std::exit(2);
  }
  for (std::string line; std::getline(in, line);) {
    auto f = splitList(line);
    if (f.size() < 9 || f[0] == "profile")
      continue;
    g_baseline[{f[0], f[1], std::stoul(f[2])}] = std::stod(f[4]);
  }
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

class Report {
public:
  explicit Report(std::string profile) : _profile(std::move(profile)) {
    if (!g_opt.csv.empty())
      _csv = std::fopen(g_opt.csv.c_str(), "a");
  }
  ~Report() {
    if (_csv)
      std::fclose(_csv);
  }
  Report(Report const&) = delete;
  Report& operator=(Report const&) = delete;

  void series(std::string_view op, std::size_t cursors, std::vector<double> const& us) {
    if (us.empty())
      return;
    auto s = statsOf(us);
    std::printf("  %-34.*s x%-2zu n=%-4zu med %10.1f  p95 %10.1f  max %10.1f us", int(op.size()),
                op.data(), cursors, s.n, s.median, s.p95, s.max);
    compare(op, cursors, s.median);
    csvRow(op, cursors, s, "us");
  }

  void value(std::string_view op, double v, char const* unit) {
    std::printf("  %-34.*s %35s %10.1f %s", int(op.size()), op.data(), "", v, unit);
    compare(op, 0, v);
    csvRow(op, 0, {1, v, v, v, v}, unit);
  }

  void failure(std::string_view op, std::size_t cursors, std::size_t done, char const* what) {
    std::printf("  %-34.*s x%-2zu FAILED (%s) after %zu calls\n", int(op.size()), op.data(),
                cursors, what, done);
    if (_csv)
      std::fprintf(_csv, "%s,%.*s,%zu,%zu,,,,,%s\n", _profile.c_str(), int(op.size()), op.data(),
                   cursors, done, what);
  }

private:
  void compare(std::string_view op, std::size_t cursors, double v) {
    auto it = g_baseline.find({_profile, std::string(op), cursors});
    if (it != g_baseline.end() && it->second > 0) {
      // Flag changes beyond 10% that are also beyond timer/RSS noise
      // (1 us or 1 MB).
      double r = v / it->second;
      bool big = std::abs(v - it->second) > 1.0;
      std::printf("  base %10.1f  %5.2fx %s", it->second, r,
                  !big      ? ""
                  : r > 1.1 ? "+"
                  : r < 0.9 ? "-"
                            : "");
    }
    std::printf("\n");
    std::fflush(stdout);
  }

  void csvRow(std::string_view op, std::size_t cursors, Stats const& s, char const* unit) {
    if (_csv)
      std::fprintf(_csv, "%s,%.*s,%zu,%zu,%.2f,%.2f,%.2f,%.2f,%s\n", _profile.c_str(),
                   int(op.size()), op.data(), cursors, s.n, s.median, s.p95, s.max, s.mean, unit);
  }

  std::string _profile;
  std::FILE* _csv = nullptr;
};

// ---------------------------------------------------------------------------
// Profiles: file shapes and their generated content
// ---------------------------------------------------------------------------

enum class Shape {
  Text,  // "<n>: lorem ipsum ..." padded to lineLen; "needle" every 1000th line,
         // or in the middle of every line when lines are long
  Cpp,   // C++-like: 10-line functions, "needle(...)" every 1000th line
  Short, // rotating "ipsum", "", "ab", "dolor", "x"; "needle" every 1000th line
};

struct Profile {
  char const* name;
  char const* what;
  Shape shape;
  std::size_t lines;
  std::size_t lineLen; // Text only
  std::size_t iters;   // keystroke-sized operations
  std::size_t heavy;   // whole-buffer operations
  bool quick;
};

constexpr Profile kProfiles[] = {
    {"small", "typical source file", Shape::Text, 1'000, 60, 300, 30, true},
    {"cpp", "C++ source (syntax highlighting, brackets)", Shape::Cpp, 20'000, 0, 300, 20, true},
    {"medium", "large source file", Shape::Text, 50'000, 60, 200, 10, true},
    {"large", "61 MB log-like file", Shape::Text, 1'000'000, 60, 40, 3, false},
    {"short", "millions of short lines", Shape::Short, 2'000'000, 0, 40, 3, false},
    {"long", "a few 500 KB lines", Shape::Text, 20, 500'000, 100, 5, false},
    {"oneline", "one 4 MB line (minified)", Shape::Text, 1, 4'000'000, 100, 5, false},
};

constexpr std::string_view kFiller =
    "lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor "
    "incididunt ut labore et dolore magna aliqua ";

bool sparseNeedle(std::size_t i) { return i % 1000 == 500; }

std::string makeLine(Profile const& p, std::size_t i) {
  switch (p.shape) {
  case Shape::Short: {
    static constexpr char const* kWords[] = {"ipsum", "", "ab", "dolor", "x"};
    return sparseNeedle(i) ? "needle" : kWords[i % 5];
  }
  case Shape::Cpp: {
    auto n = std::to_string(i);
    if (i % 10 == 0)
      return "void fn_" + n + "(int a) {";
    if (i % 10 == 9)
      return "}";
    if (sparseNeedle(i + 5)) // a body line, not a function header
      return "  needle(" + n + "); // ipsum";
    return "  int var_" + n + " = a * " + std::to_string(i * 17) + "; // ipsum comment " + n;
  }
  case Shape::Text:
    break;
  }
  std::string line = std::to_string(i) + ": ";
  if (p.lineLen < 4096 && sparseNeedle(i))
    line += "needle ";
  while (line.size() < p.lineLen)
    line += kFiller;
  line.resize(p.lineLen);
  if (p.lineLen >= 4096)
    line.replace(p.lineLen / 2, 7, " needle");
  return line;
}

std::string makeText(Profile const& p, std::size_t first, std::size_t count) {
  std::string out;
  for (std::size_t i = first; i < first + count; ++i) {
    out += makeLine(p, i);
    out += '\n';
  }
  return out;
}

// ---------------------------------------------------------------------------
// Sessions and positions
// ---------------------------------------------------------------------------

struct Session {
  TextBuffer buf;
  BufferView view{buf};
  explicit Session(std::filesystem::path const& p) : buf(p.string(), p) {}
};

// Lines with content: a file ending in '\n' has an empty last line.
std::size_t contentLines(Buffer const& b) {
  auto n = b.lineCount();
  return n > 1 && b.lineContent(n - 1).empty() ? n - 1 : n;
}

BufferPoint midPoint(Buffer const& b) {
  auto line = (contentLines(b) - 1) / 2;
  return {line, b.lineContent(line).size() / 2};
}

BufferPoint endPoint(Buffer const& b) {
  auto last = b.lineCount() - 1;
  return {last, b.lineContent(last).size()};
}

// The span between fractions @p lo and @p hi of the content lines, or of
// the middle line when there are too few lines.
std::pair<BufferPoint, BufferPoint> fractionRegion(Buffer const& b, double lo, double hi) {
  auto n = contentLines(b);
  if (n >= 4)
    return {{static_cast<std::size_t>(lo * double(n)), 0},
            {static_cast<std::size_t>(hi * double(n)), 0}};
  auto line = (n - 1) / 2;
  auto len = double(b.lineContent(line).size());
  return {{line, static_cast<std::size_t>(lo * len)}, {line, static_cast<std::size_t>(hi * len)}};
}

// About @p count lines centred in the buffer (or the middle half of the
// middle line when there are too few lines).
std::pair<BufferPoint, BufferPoint> centredRegion(Buffer const& b, std::size_t count) {
  auto n = contentLines(b);
  if (n < 4)
    return fractionRegion(b, 0.25, 0.75);
  count = std::min(count, n / 2);
  auto first = (n - count) / 2;
  return {{first, 0}, {first + count, 0}};
}

// @p k cursor positions: one per line (adjacent: consecutive lines from the
// middle; spread: evenly over the file), each in the middle of its line.
// With too few lines they go on the middle line instead (adjacent: 8 bytes
// apart; spread: evenly over the line).
std::vector<BufferPoint> cursorPositions(Buffer const& b, std::size_t k, bool spread) {
  auto n = contentLines(b);
  std::vector<BufferPoint> out;
  auto mid = (n - 1) / 2;
  bool perLine = spread ? n >= k : mid + k <= n;
  for (std::size_t j = 0; j < k; ++j) {
    if (perLine) {
      auto line = spread ? (2 * j + 1) * n / (2 * k) : mid + j;
      out.push_back({line, b.lineContent(line).size() / 2});
    } else {
      auto len = b.lineContent(mid).size();
      out.push_back({mid, std::min(len, spread ? (2 * j + 1) * len / (2 * k) : len / 2 + 8 * j)});
    }
  }
  return out;
}

void placeCursors(BufferView& v, std::vector<BufferPoint> const& pts) {
  v.setPoint(pts.front().line, pts.front().offset); // also collapses
  v.clearMark();
  for (std::size_t i = 1; i < pts.size(); ++i)
    v.addCursor(pts[i]);
}

// Mirrors Screen::findNext: smart-case search forward from @p at, wrapping
// once around the buffer. Returns the end of the match.
std::optional<BufferPoint> smartFindNext(Buffer const& b, BufferPoint at,
                                         std::string const& pattern) {
  FindFlags f(pattern);
  auto n = b.lineCount();
  for (std::size_t k = 0; k < n; ++k) {
    auto li = (at.line + k) % n;
    if (auto m = findFirstMatch(b.lineContent(li), k == 0 ? at.offset : 0, pattern, f))
      return BufferPoint{li, m->second};
  }
  if (auto m = findFirstMatch(b.lineContent(at.line), 0, pattern, f); m && m->first < at.offset)
    return BufferPoint{at.line, m->second};
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

struct Ctx {
  Profile const& prof;
  std::filesystem::path file;
  Report& rep;
  // The operation in progress, for reporting an out-of-memory abort.
  std::string currentOp;
  std::size_t currentCursors = 1;
  std::size_t currentDone = 0; // calls completed

  std::unique_ptr<Session> open() const { return std::make_unique<Session>(file); }
  void begin(std::string_view op, std::size_t cursors = 1) {
    currentOp = op;
    currentCursors = cursors;
    currentDone = 0;
  }
};

// Run @p body; an allocation failure is reported against the operation in
// progress and ends @p body (its session may be half-edited), but not the
// caller.
template <class F> void guarded(Ctx& c, F body) {
  try {
    body();
  } catch (std::bad_alloc const&) {
    c.rep.failure(c.currentOp, c.currentCursors, c.currentDone, "bad_alloc");
  }
}

template <class F> void callWith(F& f, std::size_t i) {
  if constexpr (std::invocable<F&, std::size_t>)
    f(i);
  else
    f();
}

// Time @p n calls of @p step, running @p prep untimed before each. Both may
// take the iteration index.
template <class Prep, class Step>
void series(Ctx& c, std::string_view op, std::size_t cursors, std::size_t n, Prep prep, Step step) {
  if (!wanted(op) || n == 0)
    return;
  c.begin(op, cursors);
  std::vector<double> us;
  us.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    callWith(prep, i);
    auto t0 = Clock::now();
    callWith(step, i);
    us.push_back(sinceUs(t0));
    c.currentDone = us.size();
  }
  c.rep.series(op, cursors, us);
}

template <class Step>
void series(Ctx& c, std::string_view op, std::size_t cursors, std::size_t n, Step step) {
  series(c, op, cursors, n, [] {}, std::move(step));
}

// Run one group of operations with its own peak-RSS window. An allocation
// failure ends the group but not the profile.
template <class F> void group(Ctx& c, std::string_view prefix, F body) {
  if (!groupWanted(prefix))
    return;
#ifdef __GLIBC__
  malloc_trim(0); // so the group's starting RSS is what is live, not retained heap
#endif
  double start = rssMb();
  resetPeakRss();
  c.begin(std::string(prefix) + "(setup)");
  guarded(c, body);
  c.rep.value(std::string(prefix) + "peak-growth", peakRssMb() - start, "MB");
}

// ---------------------------------------------------------------------------
// Groups
// ---------------------------------------------------------------------------

void benchIo(Ctx& c) {
  double before = rssMb();
  auto s = c.open();
  double mb = rssMb() - before;
  if (wanted("io.rss-after-open")) {
    c.rep.value("io.rss-after-open", mb, "MB");
    c.rep.value("io.bytes-per-line", mb * 1024 * 1024 / double(s->buf.lineCount()), "B");
  }
  series(c, "io.save", 1, c.prof.heavy, [&] { (void)s->buf.save(); });
  s.reset();
  series(c, "io.open", 1, c.prof.heavy, [&] {
    TextBuffer t(c.file.string(), c.file);
    g_sink = t.lineCount();
  });
}

void benchEdit(Ctx& c) {
  auto s = c.open();
  auto& v = s->view;
  auto n = c.prof.iters;
  auto mid = midPoint(s->buf);
  v.setPoint(mid.line, mid.offset);
  series(c, "edit.type", 1, n, [&] { v.insert('x'); });
  series(c, "edit.backspace", 1, n, [&] { v.deleteBeforePoint(); });
  series(c, "edit.enter", 1, n, [&] { v.insert('\n'); });
  series(c, "edit.join", 1, n, [&] { v.moveHome(); }, [&] { v.deleteBeforePoint(); });
  series(c, "edit.delete", 1, n, [&] { v.deleteAtPoint(); });
  series(c, "edit.word-delete", 1, n, [&] { v.deleteChunkBeforePoint(); });
  series(c, "edit.kill-line", 1, std::min(n, contentLines(s->buf) / 4 + 1),
         [&] { v.deleteLine(); });
}

void benchRegion(Ctx& c) {
  auto s = c.open();
  auto& v = s->view;
  auto select = [&](std::pair<BufferPoint, BufferPoint> r) {
    v.setPoint(r.second.line, r.second.offset);
    v.setMark(r.first);
  };
  auto r1k = centredRegion(s->buf, 1000);
  auto n = 2 * c.prof.heavy;
  // Toggling twice restores the text, as does upper- then lowercasing.
  series(c, "region.comment-1k", 1, n, [&] { select(r1k); }, [&] { v.toggleComment("// "); });
  series(
      c, "region.case-1k", 1, n, [&] { select(r1k); },
      [&](std::size_t i) {
        v.convertCase(i % 2 ? BufferView::CaseOp::Lower : BufferView::CaseOp::Upper);
      });
  auto r100 = centredRegion(s->buf, 100);
  series(
      c, "region.reflow-100", 1, c.prof.heavy, [&] { select(r100); },
      [&] { v.reflowParagraph(72); });
}

void benchMove(Ctx& c) {
  auto s = c.open();
  auto& b = s->buf;
  auto& v = s->view;
  RowIdx const rows{50};
  ColIdx const cols{120};
  auto n = 2 * c.prof.iters;
  auto toMid = [&] {
    auto m = midPoint(b);
    v.setPoint(m.line, m.offset);
    v.ensureVisible(rows, cols, v.wrap());
  };
  // Vertical moves restart from the middle often enough never to reach
  // either end of the buffer.
  auto every = [&](std::size_t period) {
    return [&, period = std::max<std::size_t>(1, period)](std::size_t i) {
      if (i % period == 0)
        toMid();
    };
  };
  auto quarter = contentLines(b) / 4;
  auto once = every(n);

  series(c, "move.right", 1, n, once, [&] { v.moveRight(); });
  series(c, "move.left", 1, n, once, [&] { v.moveLeft(); });
  series(c, "move.word-right", 1, n, once, [&] { v.moveWordRight(); });
  series(c, "move.word-left", 1, n, once, [&] { v.moveWordLeft(); });
  series(c, "move.down", 1, n, every(quarter), [&] { v.moveDown(cols); });
  series(c, "move.up", 1, n, every(quarter), [&] { v.moveUp(cols); });
  auto downThen = [&, step = every(quarter)](std::size_t i) mutable {
    step(i);
    v.movePhysicalDown();
  };
  series(c, "move.end", 1, n, downThen, [&] { v.moveEnd(); });
  series(c, "move.home", 1, n, downThen, [&] { v.moveHome(); });
  series(c, "move.page-down", 1, n, every(quarter / 50), [&] { v.pageDown(rows, cols, v.wrap()); });
  series(c, "move.page-up", 1, n, every(quarter / 50), [&] { v.pageUp(rows, cols, v.wrap()); });
  series(c, "move.para-down", 1, n, every(quarter / 5), [&] { v.moveParagraphDown(); });
  series(c, "move.para-up", 1, n, every(quarter / 5), [&] { v.moveParagraphUp(); });

  v.setWrap(true);
  series(c, "move.down-wrap", 1, n, every(quarter), [&] { v.moveDown(cols); });
  series(c, "move.up-wrap", 1, n, every(quarter), [&] { v.moveUp(cols); });
  series(c, "move.page-down-wrap", 1, n, every(quarter / 50),
         [&] { v.pageDown(rows, cols, true); });
  series(c, "move.click-wrap", 1, n, once, [&] {
    auto p = v.pointForScreenRowCol(25, 60, cols, true);
    g_sink = p.offset;
  });
  v.setWrap(false);

  std::mt19937 rng(42);
  std::uniform_int_distribution<std::size_t> anyLine(1, b.lineCount());
  series(c, "goto.random", 1, n, [&] {
    v.gotoLine(anyLine(rng), rows);
    v.ensureVisible(rows, cols, false);
  });
  series(c, "goto.far", 1, n, [&](std::size_t i) {
    v.gotoLine(i % 2 ? 1 : b.lineCount(), rows);
    v.ensureVisible(rows, cols, false);
  });

  // Brackets exist only in the cpp profile.
  for (auto line = midPoint(b).line; line < std::min(b.lineCount(), midPoint(b).line + 20);
       ++line) {
    if (auto off = b.lineContent(line).find('{'); off != std::string::npos) {
      series(
          c, "move.bracket", 1, n, [&] { v.setPoint(line, off); },
          [&] { g_sink = v.gotoMatchingBracket(); });
      break;
    }
  }
}

void benchSearch(Ctx& c) {
  auto s = c.open();
  auto& b = s->buf;
  auto scans = 3 * c.prof.heavy;
  auto mid = midPoint(b);
  std::string err;
  auto absentRe = compileRegex("zq[x]jv", err);
  auto needleRe = compileRegex("ne+dle", err);

  series(c, "search.smart.absent", 1, scans,
         [&] { g_sink = smartFindNext(b, mid, "zqxjv").has_value(); });
  series(c, "search.exact.absent", 1, scans,
         [&] { g_sink = findNextMatchInBuffer(b, {0, 0}, "zqxjv").has_value(); });
  series(c, "search.regex.absent", 1, scans,
         [&] { g_sink = findNextRegexInBuffer(b, {0, 0}, *absentRe).has_value(); });

  // Step through successive matches, as repeated find-next does.
  BufferPoint at{0, 0};
  auto stepSmart = [&](std::string const& pattern) {
    return [&, pattern] { at = smartFindNext(b, at, pattern).value_or(at); };
  };
  series(c, "search.smart.sparse-next", 1, c.prof.iters, stepSmart("needle"));
  series(c, "search.smart.dense-next", 1, c.prof.iters, stepSmart("ipsum"));
  at = {0, 0};
  series(c, "search.regex.sparse-next", 1, c.prof.iters, [&] {
    auto m = findNextRegexInBuffer(b, at, *needleRe);
    at = m ? m->second : BufferPoint{0, 0}; // no wrap in the helper: restart at the top
  });
}

void benchReplace(Ctx& c) {
  std::unique_ptr<Session> s;
  auto fresh = [&] {
    s.reset();
    s = c.open();
  };
  auto n = c.prof.heavy;
  std::string err;
  auto needleRe = compileRegex("ne+dle", err);
  auto ipsumRe = compileRegex("ip(s)um", err);
  // replaceAll* edit the Buffer directly, without recording a version
  // (as Screen calls them).
  series(c, "replace.all.sparse", 1, n, fresh,
         [&] { g_sink = replaceAllForward(s->buf, {0, 0}, "needle", "pin"); });
  series(c, "replace.all.dense", 1, n, fresh,
         [&] { g_sink = replaceAllForward(s->buf, {0, 0}, "ipsum", "IPSUM"); });
  series(c, "replace.all.newline", 1, n, fresh,
         [&] { g_sink = replaceAllForward(s->buf, {0, 0}, "needle", "nee\ndle"); });
  series(c, "replace.regex.sparse", 1, n, fresh,
         [&] { g_sink = replaceAllRegexForward(s->buf, {0, 0}, *needleRe, "pin"); });
  series(c, "replace.regex.dense", 1, n, fresh,
         [&] { g_sink = replaceAllRegexForward(s->buf, {0, 0}, *ipsumRe, "$1"); });

  // Interactive replace, one 'y' answer per call (Screen::replacePump plus
  // the confirm prompt): find, delete, insert. Replacing "needle" with
  // itself keeps every call doing the same work.
  if (!wanted("replace.step"))
    return;
  fresh();
  auto& v = s->view;
  series(c, "replace.step", 1, c.prof.iters, [&] {
    auto m = findNextMatchInBuffer(s->buf, v.point(), "needle");
    if (!m)
      m = findNextMatchInBuffer(s->buf, {0, 0}, "needle");
    if (!m)
      return;
    v.deleteRegion(m->first, m->second);
    auto after = v.insertText("needle");
    v.setPoint(after.line, after.offset);
  });
}

// Keystrokes measured at 1..20 cursors. Each op starts from freshly placed
// cursors (within an op they move with the edits), except those marked as
// continuing where the previous op left off: pairs undo each other so the
// buffer keeps its shape.
struct MultiOp {
  char const* name;
  bool place;
  void (*step)(BufferView&);
};
constexpr MultiOp kMultiOps[] = {
    {"multi.type", true, [](BufferView& v) { v.insert('x'); }},
    {"multi.backspace", false, [](BufferView& v) { v.deleteBeforePoint(); }},
    {"multi.enter", true, [](BufferView& v) { v.insert('\n'); }},
    {"multi.join", false, [](BufferView& v) { v.deleteBeforePoint(); }},
    {"multi.paste-word", true, [](BufferView& v) { v.insertText("ipsum "); }},
    {"multi.word-delete", false, [](BufferView& v) { v.deleteChunkBeforePoint(); }},
    {"multi.right", true, [](BufferView& v) { v.moveRight(); }},
    {"multi.down", true, [](BufferView& v) { v.moveDown(ColIdx{120}); }},
};

void benchMulti(Ctx& c) {
  auto n = std::max<std::size_t>(10, c.prof.iters / 2);
  for (auto k : g_opt.cursors) {
    for (bool spread : {false, true}) {
      std::string tag = spread ? ".spread" : ".adjacent";
      if (std::ranges::none_of(kMultiOps, [&](auto const& op) { return wanted(op.name + tag); }))
        continue;
      // A fresh session per layout, so one running out of memory does not
      // cost the others their numbers.
      c.begin("multi" + tag + " (setup)", k);
      guarded(c, [&] {
        auto s = c.open();
        auto& v = s->view;
        for (auto const& op : kMultiOps) {
          auto name = op.name + tag;
          if (!wanted(name))
            continue;
          if (op.place)
            placeCursors(v, cursorPositions(s->buf, k, spread));
          series(c, name, v.cursorCount(), n, [&] { op.step(v); });
        }
      });
    }
  }
}

void benchClip(Ctx& c) {
  auto s = c.open();
  auto& b = s->buf;
  auto& v = s->view;
  auto& p = c.prof;

  // Paste @p text at the start of the middle line, then cut it again (as
  // Screen's Paste and Cut do), timing both.
  auto pasteCut = [&](std::string const& size, std::string const& text, std::size_t n) {
    auto pasteOp = "clip.paste-" + size, cutOp = "clip.cut-" + size;
    if (!wanted(pasteOp) && !wanted(cutOp))
      return;
    std::vector<double> pasted, cut;
    for (std::size_t i = 0; i < n; ++i) {
      auto at = BufferPoint{midPoint(b).line, 0};
      v.setPoint(at.line, at.offset);
      c.begin(pasteOp);
      c.currentDone = i;
      auto t0 = Clock::now();
      auto end = v.insertText(text);
      pasted.push_back(sinceUs(t0));
      c.begin(cutOp);
      c.currentDone = i;
      t0 = Clock::now();
      g_sink = v.extractRegion(at, end).size();
      v.deleteRegion(at, end);
      cut.push_back(sinceUs(t0));
    }
    c.rep.series(pasteOp, 1, pasted);
    c.rep.series(cutOp, 1, cut);
  };
  // Payloads of ordinary 60-byte lines, whatever the file's shape.
  constexpr Profile kPayload{"", "", Shape::Text, 0, 60, 0, 0, false};
  pasteCut("1k-lines", makeText(kPayload, 0, 1000), std::max<std::size_t>(3, p.iters / 4));
  pasteCut("100k-lines", makeText(kPayload, 0, 100'000), p.heavy);
  std::string mbLine;
  while (mbLine.size() < (1 << 20))
    mbLine += kFiller;
  pasteCut("1MB-line", mbLine, p.heavy);

  auto half = fractionRegion(b, 0.25, 0.75);
  series(c, "clip.copy-half", 1, p.heavy,
         [&] { g_sink = v.extractRegion(half.first, half.second).size(); });

  // Cut a region and paste it back, timing both; restores the text.
  auto cutPaste = [&](std::string const& size, std::pair<BufferPoint, BufferPoint> r) {
    auto cutOp = "clip.cut-" + size, pasteOp = "clip.paste-" + size;
    if (!wanted(cutOp) && !wanted(pasteOp))
      return;
    std::vector<double> cut, pasted;
    for (std::size_t i = 0; i < p.heavy; ++i) {
      c.begin(cutOp);
      c.currentDone = i;
      auto t0 = Clock::now();
      auto text = v.extractRegion(r.first, r.second);
      v.deleteRegion(r.first, r.second);
      cut.push_back(sinceUs(t0));
      c.begin(pasteOp);
      c.currentDone = i;
      v.setPoint(r.first.line, r.first.offset);
      t0 = Clock::now();
      v.insertText(text);
      pasted.push_back(sinceUs(t0));
    }
    c.rep.series(cutOp, 1, cut);
    c.rep.series(pasteOp, 1, pasted);
  };
  cutPaste("half", half);
  cutPaste("all", {{0, 0}, endPoint(b)});
}

void benchRender(Ctx& c) {
  auto s = c.open();
  auto& b = s->buf;
  auto& v = s->view;
  std::string out;
  struct Case {
    char const* op;
    std::size_t rows, cols;
    char where; // t(op), m(iddle), e(nd)
    bool wrap, gutter;
  };
  constexpr Case kCases[] = {
      {"render.80x24.top", 24, 80, 't', false, false},
      {"render.200x60.mid", 60, 200, 'm', false, false},
      {"render.200x60.mid-wrap-gutter", 60, 200, 'm', true, true},
      {"render.200x60.end", 60, 200, 'e', false, false},
  };
  for (auto const& k : kCases) {
    if (!wanted(k.op))
      continue;
    auto p = k.where == 't' ? BufferPoint{0, 0} : k.where == 'm' ? midPoint(b) : endPoint(b);
    v.setPoint(p.line, p.offset);
    v.setWrap(k.wrap);
    v.setShowLineNumbers(k.gutter);
    v.ensureVisible(RowIdx{k.rows}, ColIdx{k.cols}, k.wrap);
    series(c, k.op, 1, c.prof.iters, [&] {
      out.clear();
      v.render(out, RowIdx{k.rows}, ColIdx{k.cols});
    });
  }
  v.setWrap(false);
  v.setShowLineNumbers(false);

  // A keystroke as the user sees it: edit, scroll, redraw (re-tokenizes
  // from the edited line in highlighted modes).
  RowIdx const rows{60};
  ColIdx const cols{200};
  auto mid = midPoint(b);
  series(
      c, "render.type+frame", 1, c.prof.iters, [&] { v.setPoint(mid.line, mid.offset); },
      [&] {
        v.insert('x');
        v.ensureVisible(rows, cols, false);
        out.clear();
        v.render(out, rows, cols);
      });

  // First frame at the end of a freshly opened file (cold syntax cache).
  std::unique_ptr<Session> f;
  series(
      c, "render.cold-goto-end", 1, c.prof.heavy,
      [&] {
        f.reset();
        f = c.open();
      },
      [&] {
        f->view.gotoLine(f->buf.lineCount(), rows);
        f->view.ensureVisible(rows, cols, false);
        out.clear();
        f->view.render(out, rows, cols);
      });
}

// Keystroke bursts long enough to fill the history past its cap
// (VersionHistory::kMaxVersions) and trigger pruning.
constexpr std::size_t kBurst = 300;

void benchBurst(Ctx& c, std::string const& op, std::size_t cursors, uint32_t key) {
  auto s = c.open();
  auto& v = s->view;
  placeCursors(v, cursorPositions(s->buf, cursors, true));
  double before = rssMb();
  series(c, op + ".keystroke", v.cursorCount(), kBurst, [&] { v.insert(key); });
  c.rep.value(op + ".rss-growth", rssMb() - before, "MB");
  c.rep.value(op + ".versions", double(s->buf.versionHistoryInfo().size), "count");
}

void benchBrowse(Ctx& c) {
  auto s = c.open();
  auto& v = s->view;
  auto mid = midPoint(s->buf);
  v.setPoint(mid.line, mid.offset);
  c.begin("history.browse (setup)");
  for (std::size_t i = 0; i < 150; ++i) // 150 versions: past one prune
    v.insert(i % 2 ? '\n' : 'x');
  auto steps = s->buf.versionHistoryInfo().size - 1;
  series(c, "history.browse.enter", 1, 1, [&] { v.enterHistoryMode(); });
  series(c, "history.browse.back", 1, steps, [&] { v.stepHistory(-1); });
  series(c, "history.browse.forward", 1, steps, [&] { v.stepHistory(+1); });
  v.exitHistoryModeCommit();
}

void runProfile(Profile const& p) {
  auto suffix = p.shape == Shape::Cpp ? ".cpp" : ".txt";
  auto file = std::make_unique<TempFile>(makeText(p, 0, p.lines), suffix);
  auto bytes = std::filesystem::file_size(file->path());
  std::printf("\n=== %s: %s — %zu lines, %.1f MB ===\n", p.name, p.what, p.lines,
              double(bytes) / (1024 * 1024));
  Report rep(p.name);
  Ctx c{p, file->path(), rep};

  group(c, "io.", [&] { benchIo(c); });
  group(c, "edit.", [&] { benchEdit(c); });
  group(c, "region.", [&] { benchRegion(c); });
  group(c, "move.", [&] { benchMove(c); }); // includes goto.*
  group(c, "search.", [&] { benchSearch(c); });
  group(c, "multi.", [&] { benchMulti(c); });
  group(c, "clip.", [&] { benchClip(c); });
  group(c, "replace.", [&] { benchReplace(c); });
  group(c, "render.", [&] { benchRender(c); });
  group(c, "history.browse.", [&] { benchBrowse(c); });
  group(c, "history.type.", [&] { benchBurst(c, "history.type", 1, 'x'); });
  group(c, "history.enter.", [&] { benchBurst(c, "history.enter", 1, '\n'); });
  auto k = std::ranges::max(g_opt.cursors);
  group(c, "history.multi.", [&] { benchBurst(c, "history.multi", k, 'x'); });
}

void usage() {
  std::printf("usage: bench_suite [--quick] [--profile NAME,...] [--filter PREFIX,...]\n"
              "                   [--cursors N,...] [--csv FILE] [--compare FILE]\n"
              "                   [--mem-limit MB] [--list]\n"
              "See the comment at the top of tests/bench_suite.cpp.\n");
}

} // namespace

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--quick")
      g_opt.quick = true;
    else if (a == "--profile")
      g_opt.profiles = splitList(next());
    else if (a == "--filter")
      g_opt.filters = splitList(next());
    else if (a == "--cursors") {
      g_opt.cursors.clear();
      for (auto const& k : splitList(next()))
        g_opt.cursors.push_back(std::stoul(k));
    } else if (a == "--csv")
      g_opt.csv = next();
    else if (a == "--compare")
      g_opt.compare = next();
    else if (a == "--mem-limit")
      g_opt.memLimitMb = std::stoul(next());
    else if (a == "--list") {
      for (auto const& p : kProfiles)
        std::printf("%-8s %-44s %9zu lines%s\n", p.name, p.what, p.lines,
                    p.quick ? "  (quick)" : "");
      return 0;
    } else {
      usage();
      return a == "--help" || a == "-h" ? 0 : 2;
    }
  }
  if (g_opt.cursors.empty() ||
      std::ranges::any_of(g_opt.cursors, [](auto k) { return k < 1 || k > 20; })) {
    std::fprintf(stderr, "--cursors: counts must be 1..20\n");
    return 2;
  }
  for (auto const& name : g_opt.profiles)
    if (std::ranges::none_of(kProfiles, [&](auto const& p) { return name == p.name; })) {
      std::fprintf(stderr, "unknown profile %s (see --list)\n", name.c_str());
      return 2;
    }
  if (!g_opt.compare.empty())
    loadBaseline(g_opt.compare);
  if (!g_opt.csv.empty()) {
    std::ofstream(g_opt.csv) << kCsvHeader << '\n';
  }

  int failures = 0;
  for (auto const& p : kProfiles) {
    bool selected = g_opt.profiles.empty()
                        ? !g_opt.quick || p.quick
                        : std::ranges::find(g_opt.profiles, p.name) != g_opt.profiles.end();
    if (!selected)
      continue;
    std::fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      if (g_opt.memLimitMb > 0) {
        rlim_t bytes = rlim_t(g_opt.memLimitMb) << 20;
        rlimit lim{bytes, bytes};
        setrlimit(RLIMIT_AS, &lim);
      }
      runProfile(p);
      std::fflush(stdout);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::printf("  profile %s: child %s %d\n", p.name,
                  WIFSIGNALED(status) ? "killed by signal" : "exited with",
                  WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
      ++failures;
    }
  }
  return failures ? 1 : 0;
}
