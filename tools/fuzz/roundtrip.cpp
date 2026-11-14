// Load/save round trip: 30,000 small files (named edge cases, then random
// bytes with CR, NUL, invalid UTF-8, missing final newline) are loaded and saved
// unchanged; any byte difference is printed, grouped by kind.
// usage: roundtrip   (works in $TMPDIR/rt)
#include "buffer/text_buffer.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
static std::string slurp(std::filesystem::path const& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static std::string hex(std::string const& s) {
  std::string o;
  char b[4];
  for (std::size_t i = 0; i < s.size() && i < 24; ++i) {
    std::snprintf(b, 4, "%02x ", (unsigned char)s[i]);
    o += b;
  }
  return o + (s.size() > 24 ? "..." : "");
}
int main() {
  auto dir = std::filesystem::path(std::getenv("TMPDIR")) / "rt";
  std::filesystem::create_directories(dir);
  auto p = dir / "f.txt";
  std::map<std::string, int> kinds;
  std::mt19937 rng(7);
  char const* named[] = {
      "",           "\n",     "a",        "a\n",     "a\n\n",       "\n\n",
      "a\r\nb\r\n", "a\rb\r", "a\r\nb\n", "a\0b\n",  "\xff\xfe\n",  "\xef\xbb\xbfx\n",
      "x\r",        "\r\n",   "\r",       "a\n\r\n", "tab\there \n"};
  std::size_t namedLen[] = {0, 1, 1, 2, 3, 2, 6, 4, 5, 4, 3, 5, 2, 2, 1, 4, 11};
  int shown = 0;
  for (int it = 0; it < 30000; ++it) {
    std::string in;
    if (it < (int)std::size(named))
      in.assign(named[it], namedLen[it]);
    else {
      char alpha[] = "ab \t\n\r\0\xc3\xa9\xff";
      for (int k = rng() % 40; k; --k)
        in += alpha[rng() % 11];
    }
    std::ofstream(p, std::ios::binary) << in;
    std::string out;
    {
      TextBuffer b("f", p);
      auto r = b.save();
      if (!r) {
        std::printf("save failed: %s\n", r.error().c_str());
        continue;
      }
      out = slurp(p);
    }
    if (out != in) {
      std::string k;
      if (in.find('\r') != std::string::npos)
        k = "CR";
      else if (in.find('\0') != std::string::npos)
        k = "NUL";
      else if (!in.empty() && in.back() != '\n')
        k = "no-final-newline";
      else
        k = "other";
      if (kinds[k]++ < 3 && shown++ < 12)
        std::printf("[%s] in:  %s\n     out: %s\n", k.c_str(), hex(in).c_str(), hex(out).c_str());
    }
  }
  for (auto& [k, n] : kinds)
    std::printf("mismatch kind %-16s x%d\n", k.c_str(), n);
}
