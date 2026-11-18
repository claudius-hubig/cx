// Targeted probes and random fuzzing of the small parsing helpers (regex and
// plain replace-all termination, grep output parser, CLI and path:LINE
// arguments, SGR mouse sequences, key escape sequences and bracketed paste,
// UTF-8 helpers). Each probe runs in a forked
// child with a timeout so hangs and crashes are reported rather than fatal.
// usage: parse_fuzz
#include "buffer/grep_buffer.hpp"
#include "buffer/text_buffer.hpp"
#include "cli.hpp"
#include "file_arg.hpp"
#include "key_sequence.hpp"
#include "mouse.hpp"
#include "regex_search.hpp"
#include "replace.hpp"
#include "search.hpp"
#include "utf8.hpp"

#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void probe(char const* name, unsigned timeoutSec, std::function<void()> fn) {
  std::fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    rlimit rl{2u << 30, 2u << 30};
    setrlimit(RLIMIT_AS, &rl);
    alarm(timeoutSec);
    fn();
    std::fflush(stdout);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  if (WIFSIGNALED(st))
    std::printf("[%s] %s: killed by signal %d%s\n", WTERMSIG(st) == SIGALRM ? "HANG " : "CRASH",
                name, WTERMSIG(st), WTERMSIG(st) == SIGALRM ? " (timeout)" : "");
  else if (WEXITSTATUS(st) != 0)
    std::printf("[EXIT ] %s: exit %d\n", name, WEXITSTATUS(st));
}

static std::unique_ptr<TextBuffer> mkbuf(std::string const& content) {
  auto b = std::make_unique<TextBuffer>("t");
  b->insertText(content, BufferPoint{0, 0});
  return b;
}

static std::mt19937_64 rng(1);
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }
static std::string randBytes(std::size_t n, char const* alpha = nullptr) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s += alpha ? alpha[R(std::strlen(alpha))] : static_cast<char>(R(256));
  return s;
}

int main() {
  // ---------------- regex: zero-width replace-all termination
  for (auto [pat, rep] : std::initializer_list<std::pair<char const*, char const*>>{{"x*", ""},
                                                                                    {"x*", "y"},
                                                                                    {"^", "> "},
                                                                                    {"$", ";"},
                                                                                    {"\\b", "|"},
                                                                                    {"(?=a)", "-"},
                                                                                    {"a", "a"},
                                                                                    {"a", "aa"}}) {
    std::string name = std::string("replaceAllRegexForward /") + pat + "/ -> \"" + rep + "\"";
    probe(name.c_str(), 3, [pat, rep] {
      auto b = mkbuf("abc\nbab\n");
      std::string err;
      auto re = compileRegex(pat, err);
      auto n = replaceAllRegexForward(*b, {0, 0}, *re, rep);
      std::printf("        %-46s replaced %zu, line0=[%s]\n", "", n,
                  b->lineContent(0).substr(0, 40).c_str());
    });
  }
  // ---------------- regex: anchors / word boundaries when searching mid-line
  probe("findFirstRegex anchors from mid-line", 3, [] {
    std::string err;
    auto check = [&](char const* pat, std::string line, std::size_t from) {
      auto re = compileRegex(pat, err);
      auto m = findFirstRegex(line, from, *re);
      std::printf("        /%s/ on \"%s\" from %zu -> %s\n", pat, line.c_str(), from,
                  m ? ("match at " + std::to_string(m->first)).c_str() : "no match");
    };
    check("^a", "ba", 1);   // ^ should not match mid-line
    check("\\bb", "ab", 1); // no word boundary between a and b
    check("(?:^|x)c", "abc", 2);
  });
  probe("findLastRegex anchors", 3, [] {
    std::string err;
    auto re = compileRegex("^a", err);
    auto m = findLastRegex("aaa", 0, 3, *re);
    std::printf("        /^a/ last in \"aaa\" -> %s\n",
                m ? ("match at " + std::to_string(m->first)).c_str() : "none");
  });
  // ---------------- regex: substituteCaptures with context-dependent match
  probe("substituteCaptures with lookahead", 3, [] {
    std::string err;
    auto re = compileRegex("(a)(?=b)", err);
    std::string line = "ab";
    auto m = findFirstRegex(line, 0, *re);
    auto out = substituteCaptures(line, m->first, m->second, *re, "[$1]");
    std::printf("        /(a)(?=b)/ on \"ab\" with \"[$1]\" -> \"%s\" (expected \"[a]\")\n",
                out.c_str());
  });
  // ---------------- regex: deep recursion / catastrophic patterns on long lines
  for (auto [pat, len] :
       std::initializer_list<std::pair<char const*, std::size_t>>{{"(a|b)*c", 20000},
                                                                  {"(a|b)*c", 200000},
                                                                  {".*x", 200000},
                                                                  {"(a*)*b", 30},
                                                                  {"(x+x+)+y", 30},
                                                                  {"a{1000}", 2000},
                                                                  {"(.)*", 100000},
                                                                  {"[a-z]*z", 200000}}) {
    std::string name =
        std::string("findFirstRegex /") + pat + "/ on " + std::to_string(len) + " chars";
    probe(name.c_str(), 5, [pat, len] {
      std::string err;
      auto re = compileRegex(pat, err);
      if (!re) {
        std::printf("        compile failed: %s\n", err.c_str());
        return;
      }
      std::string line(len, pat[1] == 'x' ? 'x' : 'a');
      auto m = findFirstRegex(line, 0, *re);
      (void)m;
    });
  }
  // ---------------- plain replace-all
  probe("replaceAllForward overlapping", 3, [] {
    auto b = mkbuf("aaaa");
    auto n = replaceAllForward(*b, {0, 0}, "aa", "aaa");
    std::printf("        replaced %zu -> [%s]\n", n, b->lineContent(0).c_str());
  });
  // ---------------- UTF-8 layout helpers with degenerate widths
  for (std::size_t cols : {0u, 1u, 2u}) {
    for (char const* s : {"abc", "中文字", "😀😀", "a\tb", "e\xcc\x81", "\xff\xfe", ""}) {
      std::string name =
          "computeRowBounds/findVisualRow/lastVisualRowStart cols=" + std::to_string(cols) + " \"" +
          escapeNonPrintable(s) + "\"";
      probe(name.c_str(), 2, [s, cols] {
        std::string line = s;
        auto rb = computeRowBounds(line, cols);
        for (std::size_t i = 1; i < rb.size(); ++i)
          if (rb[i] <= rb[i - 1]) {
            std::printf("        non-increasing row bounds: cols=%zu [%s] ->", cols,
                        escapeNonPrintable(line).c_str());
            for (auto x : rb)
              std::printf(" %zu", x);
            std::printf("\n");
            break;
          }
        for (std::size_t off = 0; off <= line.size(); ++off)
          (void)findVisualRow(line, off, cols);
        (void)lastVisualRowStart(line, cols);
        (void)offsetForVisualCol(line, 0, line.size(), 3);
        (void)truncateVisualFromFront(line, cols);
      });
    }
  }
  // ---------------- random fuzz of parsers and decoders (in-process)
  probe("random fuzz: decodeUtf8/charInfoAt/visualWidth/escape", 60, [] {
    for (int it = 0; it < 200000; ++it) {
      auto s = randBytes(R(12));
      for (std::size_t i = 0; i < s.size();) {
        auto [cp, n] = decodeUtf8(s.data() + i, s.size() - i);
        if (n == 0 || n > s.size() - i) {
          std::printf("        decodeUtf8 bad length %zu at %zu\n", n, i);
          std::fflush(stdout);
          _exit(4);
        }
        auto ci = charInfoAt(s.data() + i, s.size() - i);
        auto di = displayCharInfoAt(s.data() + i, s.size() - i);
        (void)ci;
        (void)di;
        (void)cp;
        i += n;
      }
      (void)visualWidth(s);
      (void)escapeNonPrintable(s);
      (void)computeRowBounds(s, 1 + R(5));
    }
  });
  probe("random fuzz: parseSgrMouse", 30, [] {
    for (int it = 0; it < 300000; ++it) {
      auto s = randBytes(R(16), "0123456789;Mm<x-");
      auto e = parseSgrMouse(s);
      (void)e;
    }
  });
  probe("random fuzz: parseFileArg", 30, [] {
    for (int it = 0; it < 100000; ++it) {
      auto s = randBytes(R(12), "f:0123456789:-+ x");
      auto [p, l] = parseFileArg(s);
      if (l && *l == 0) {
        std::printf("        parseFileArg returned line 0 for %s\n", s.c_str());
        std::fflush(stdout);
        _exit(4);
      }
    }
  });
  probe("random fuzz: cli::parse against a model", 30, [] {
    char const* toks[] = {"-h",
                          "--help",
                          "--version",
                          "--",
                          "-",
                          "f",
                          "-x",
                          "--x",
                          "",
                          "--help=1",
                          "-hv",
                          "--theme=dark",
                          "--theme=light",
                          "--theme=",
                          "--theme",
                          "--theme=Light",
                          "--theme=dark=",
                          "f:3",
                          "--theme=light "};
    for (int it = 0; it < 100000; ++it) {
      std::vector<std::string_view> a;
      for (std::size_t k = R(6); k; --k)
        a.push_back(toks[R(std::size(toks))]);
      auto r = cli::parse(a);
      // The model: options until "--"; the first help/version/bad option
      // decides; otherwise every other argument is a file and the last
      // theme wins.
      enum { Files, Help, Version, Error } want = Files;
      std::vector<std::string> files;
      Theme theme = Theme::Dark;
      bool done = false;
      for (auto t : a) {
        if (!done && t.size() > 1 && t[0] == '-') {
          if (t == "--") {
            done = true;
            continue;
          }
          if (t == "-h" || t == "--help")
            want = Help;
          else if (t == "--version")
            want = Version;
          else if (t == "--theme=dark")
            theme = Theme::Dark;
          else if (t == "--theme=light")
            theme = Theme::Light;
          else
            want = Error;
          if (want != Files)
            break;
          continue;
        }
        files.emplace_back(t);
      }
      bool ok = want == Error     ? !r
                : want == Help    ? r && std::holds_alternative<cli::ShowHelp>(*r)
                : want == Version ? r && std::holds_alternative<cli::ShowVersion>(*r)
                                  : r && std::holds_alternative<cli::OpenFiles>(*r) &&
                                        std::get<cli::OpenFiles>(*r).args == files &&
                                        std::get<cli::OpenFiles>(*r).theme == theme;
      if (!ok) {
        std::printf("        cli::parse disagrees with the model for:");
        for (auto t : a)
          std::printf(" [%.*s]", static_cast<int>(t.size()), t.data());
        std::printf("\n");
        std::fflush(stdout);
        _exit(4);
      }
    }
  });
  probe("key sequences: every key and modifier round-trips; random bytes are safe", 30, [] {
    struct Shape {
      Key key;
      char final;
      char const* number; // "" for the letter form
    };
    Shape shapes[] = {{Key::Up, 'A', ""},      {Key::Down, 'B', ""},     {Key::Right, 'C', ""},
                      {Key::Left, 'D', ""},    {Key::Home, 'H', ""},     {Key::End, 'F', ""},
                      {Key::Home, '~', "1"},   {Key::Home, '~', "7"},    {Key::Insert, '~', "2"},
                      {Key::Delete, '~', "3"}, {Key::End, '~', "4"},     {Key::End, '~', "8"},
                      {Key::PageUp, '~', "5"}, {Key::PageDown, '~', "6"}};
    for (auto const& sh : shapes) {
      for (unsigned m = 1; m <= 8; ++m) {
        std::string params = *sh.number ? sh.number : "1";
        if (m > 1 || R(2))
          params += ";" + std::to_string(m);
        else if (!*sh.number && R(2))
          params.clear(); // ESC [ A
        KeyPress want{sh.key, ((m - 1) & 1) != 0, ((m - 1) & 2) != 0, ((m - 1) & 4) != 0};
        auto got = decodeCsiKey(params, sh.final);
        if (!got || !(*got == want)) {
          std::printf("        decodeCsiKey(\"%s\", '%c') is wrong\n", params.c_str(), sh.final);
          std::fflush(stdout);
          _exit(4);
        }
      }
    }
    for (int it = 0; it < 300000; ++it) {
      auto params = randBytes(R(10), "0123456789;:?<>~ -");
      char fin = static_cast<char>(0x40 + R(0x3f));
      auto k = decodeCsiKey(params, fin);
      // Anything outside the two documented shapes must be refused.
      if (k && fin != '~' && std::string_view("ABCDHF").find(fin) == std::string_view::npos) {
        std::printf("        decodeCsiKey accepted final '%c' with [%s]\n", fin, params.c_str());
        std::fflush(stdout);
        _exit(4);
      }
      if (k && params.find_first_not_of("0123456789;") != std::string::npos) {
        std::printf("        decodeCsiKey accepted parameters [%s]\n", params.c_str());
        std::fflush(stdout);
        _exit(4);
      }
      (void)decodeSs3Key(fin);
      (void)isPasteStart(params, fin);
    }
  });
  probe("bracketed paste: normalizePastedText", 30, [] {
    for (int it = 0; it < 300000; ++it) {
      auto raw = randBytes(R(20), "ab\r\n\t\x1b[~2\xc3\xa9");
      auto t = normalizePastedText(raw);
      // Each "\r\n", lone '\r' and lone '\n' is one line break, and nothing
      // else changes.
      std::string want;
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
          continue;
        want += raw[i] == '\r' ? '\n' : raw[i];
      }
      if (t != want) {
        std::printf("        normalizePastedText(%s) is wrong\n", raw.c_str());
        std::fflush(stdout);
        _exit(4);
      }
    }
  });
  probe("random fuzz: parseGrepOutput", 30, [] {
    for (int it = 0; it < 100000; ++it) {
      auto s = randBytes(R(60), "ab/.:0123456789\nx");
      s += randBytes(R(4));
      auto hits = parseGrepOutput(s, "/r");
      (void)hits;
    }
  });
  probe("random fuzz: smart find matchAt/findFirst/findLast", 60, [] {
    for (int it = 0; it < 200000; ++it) {
      auto line = randBytes(R(30), "aAbB1 _-\xc3\xa9\xe4\xb8\xad\xff");
      auto pat = randBytes(1 + R(4), "aAbB1 _-\xc3\xa9");
      FindFlags f(pat);
      std::size_t from = R(line.size() + 2);
      auto m = findFirstMatch(line, from, pat, f);
      if (m && (m->first < from || m->second > line.size() || m->second < m->first)) {
        std::printf("        findFirstMatch out of range\n");
        _exit(4);
      }
      auto l = findLastMatch(line, 0, R(line.size() + 2), pat, f);
      if (l && l->second > line.size()) {
        std::printf("        findLastMatch out of range\n");
        std::fflush(stdout);
        _exit(4);
      }
    }
  });
  std::printf("parse_fuzz done\n");
}
