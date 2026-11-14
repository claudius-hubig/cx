// Find and minimise formatMarkdownTable idempotence violations.
// usage: table_min SEED ITERS KIND(1=lines,2=cursor,3=newline) LEVEL
#include "buffer/modes/md/table.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>
static std::mt19937_64 rng;
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }
using T = std::vector<std::string>;
static std::string esc(std::string_view s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 0x20 || c == 0x7f || c >= 0x80) {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    } else
      o += static_cast<char>(c);
  }
  return o;
}
static int violates(T const& t, std::optional<BufferPoint> c, std::size_t w) {
  if (std::getenv("NOTICK"))
    for (auto const& l : t)
      if (std::count(l.begin(), l.end(), '`') % 2)
        return 0;
  if (c && (c->line >= t.size() || c->offset > t[c->line].size() ||
            clusterStart(t[c->line], c->offset) != c->offset))
    return 0;
  auto r = formatMarkdownTable(t, c, w);
  if (!r)
    return 0;
  for (auto const& l : r->lines)
    if (l.find('\n') != std::string::npos)
      return 3;
  auto r2 = formatMarkdownTable(r->lines, r->cursor, w);
  if (!r2)
    return 4;
  if (r2->lines != r->lines)
    return 1;
  if (r2->cursor != r->cursor)
    return 2;
  return 0;
}
static void show(T const& t, std::optional<BufferPoint> c, std::size_t w) {
  auto r = formatMarkdownTable(t, c, w);
  auto r2 = formatMarkdownTable(r->lines, r->cursor, w);
  std::printf("width=%zu cursor=%s\n input:\n", w,
              c ? (std::to_string(c->line) + ":" + std::to_string(c->offset)).c_str() : "none");
  for (auto const& l : t)
    std::printf("   [%s]\n", esc(l).c_str());
  std::printf(" pass 1 (cursor %zu:%zu):\n", r->cursor ? r->cursor->line : 0,
              r->cursor ? r->cursor->offset : 0);
  for (auto const& l : r->lines)
    std::printf("   [%s]\n", esc(l).c_str());
  std::printf(" pass 2 (cursor %zu:%zu):\n", r2->cursor ? r2->cursor->line : 0,
              r2->cursor ? r2->cursor->offset : 0);
  for (auto const& l : r2->lines)
    std::printf("   [%s]\n", esc(l).c_str());
}
int main(int argc, char** argv) {
  unsigned long long seed = std::strtoull(argv[1], nullptr, 10),
                     iters = std::strtoull(argv[2], nullptr, 10);
  int kind = std::atoi(argv[3]), level = std::atoi(argv[4]);
  rng.seed(seed);
  static char const* plain[] = {
      "a", "bb", "word", " ", " ", " ", "longerword", "supercalifragilistic", "x-y", "1."};
  static char const* rich[] = {
      "`c d`", "[t t](u u)", "\xe4\xb8\xad\xe6\x96\x87", "e\xcc\x81", "\\|", "*", "`"};
  static char const* odd[] = {
      "\t", "\xc2\x85", "\xe2\x80\x8b", "\xef\xbb\xbf", "\xcc\x81", "|", "\\", "[", "](", ">", ":"};
  std::size_t best = SIZE_MAX;
  int found = 0;
  for (unsigned long long it = 0; it < iters && found < 3; ++it) {
    std::size_t ncol = 1 + R(4);
    T t;
    auto cell = [&] {
      std::string s;
      for (std::size_t n = R(4) == 0 ? 0 : R(R(6) == 0 ? 30 : 6); n; --n)
        s += level >= 3 && R(5) == 0   ? odd[R(std::size(odd))]
             : level >= 2 && R(4) == 0 ? rich[R(std::size(rich))]
                                       : plain[R(std::size(plain))];
      return s;
    };
    auto row = [&](bool cont) {
      std::string s = "|";
      for (std::size_t i = 0; i < ncol; ++i)
        s += " " + (cont && i == 0 ? std::string() : cell()) + " |";
      return s;
    };
    t.push_back(row(false));
    std::string d = "|";
    for (std::size_t i = 0; i < ncol; ++i)
      d += "---|";
    t.push_back(d);
    for (std::size_t n = 1 + R(5); n; --n)
      t.push_back(row(R(4) == 0));
    std::size_t w = R(5) == 0 ? R(6) : 10 + R(100);
    std::size_t cl = R(t.size());
    std::optional<BufferPoint> c = BufferPoint{cl, clusterStart(t[cl], R(t[cl].size() + 1))};
    if (std::getenv("NOTICK")) {
      bool odd = false;
      for (auto const& l : t)
        odd = odd || (std::count(l.begin(), l.end(), '`') % 2) != 0;
      if (odd)
        continue;
    }
    if (violates(t, c, w) != kind)
      continue;
    // reduce
    bool progress = true;
    while (progress) {
      progress = false;
      for (std::size_t i = 2; i < t.size(); ++i) { // drop rows
        T u = t;
        u.erase(u.begin() + i);
        auto c2 = c;
        if (c2->line == i)
          continue;
        if (c2->line > i)
          --c2->line;
        if (violates(u, c2, w) == kind) {
          t = u;
          c = c2;
          progress = true;
          --i;
        }
      }
      for (std::size_t i = 0; i < t.size(); ++i) // drop chars
        for (std::size_t len : {8u, 4u, 2u, 1u})
          for (std::size_t p = 0; p + len <= t[i].size(); ++p) {
            T u = t;
            u[i].erase(p, len);
            auto c2 = c;
            if (c2->line == i && c2->offset > p)
              c2->offset = c2->offset >= p + len ? c2->offset - len : p;
            if (violates(u, c2, w) == kind) {
              t = u;
              c = c2;
              progress = true;
              --p;
            }
          }
      while (w > 0 && violates(t, c, w - 1) == kind) {
        --w;
        progress = true;
      }
    }
    std::size_t sz = 0;
    for (auto const& l : t)
      sz += l.size();
    ++found;
    std::printf("--- kind %d, minimised to %zu bytes ---\n", kind, sz);
    show(t, c, w);
    (void)best;
  }
}
