// Render fuzzer: hostile / crafted content in every buffer type and mode,
// rendered with random view state and geometry. Oracle: the byte stream sent
// to the terminal must be "terminal safe" — valid UTF-8, no C0/C1 control
// characters, and no escape sequence other than a well-formed CSI — so file
// content, file names and grep output can never drive the terminal.
// usage: render_fuzz SEED ITERS
#include "buffer/buffer.hpp"
#include "buffer/buffer_list_buffer.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/dir_buffer.hpp"
#include "buffer/grep_buffer.hpp"
#include "buffer/help_buffer.hpp"
#include "buffer/paste_ring_buffer.hpp"
#include "buffer/text_buffer.hpp"
#include "paste_ring.hpp"
#include "utf8.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <random>
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
    if (c < 0x20 || c == 0x7f || c >= 0x80) {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", c);
      o += b;
    } else
      o += static_cast<char>(c);
  }
  return o;
}

static std::string gCtx;
[[noreturn]] static void fail(std::string const& what, std::string const& out, std::size_t at) {
  std::size_t from = at > 80 ? at - 80 : 0;
  std::fprintf(stderr, "FAIL %s\n  %s\n  output around byte %zu: [%s]\n", gCtx.c_str(),
               what.c_str(), at, esc(std::string_view(out).substr(from, 160)).c_str());
  std::exit(3);
}

/// The only things a renderer may emit: printable UTF-8 and CSI sequences.
static void checkTerminalSafe(std::string const& out) {
  for (std::size_t i = 0; i < out.size();) {
    auto c = static_cast<unsigned char>(out[i]);
    if (c == 0x1b) {
      if (i + 1 >= out.size() || out[i + 1] != '[')
        fail("ESC not starting a CSI sequence", out, i);
      std::size_t j = i + 2;
      while (j < out.size() && static_cast<unsigned char>(out[j]) >= 0x30 &&
             static_cast<unsigned char>(out[j]) <= 0x3f)
        ++j;
      while (j < out.size() && static_cast<unsigned char>(out[j]) >= 0x20 &&
             static_cast<unsigned char>(out[j]) <= 0x2f)
        ++j;
      if (j >= out.size() || static_cast<unsigned char>(out[j]) < 0x40 ||
          static_cast<unsigned char>(out[j]) > 0x7e)
        fail("malformed CSI sequence", out, i);
      i = j + 1;
      continue;
    }
    if (c < 0x20 || c == 0x7f)
      fail("raw control byte", out, i);
    if (c < 0x80) {
      ++i;
      continue;
    }
    auto [cp, len] = decodeUtf8(out.data() + i, out.size() - i);
    if (len < 2)
      fail("invalid UTF-8 byte", out, i);
    if (cp >= 0x80 && cp <= 0x9f)
      fail("raw C1 control character", out, i);
    if (cp == 0x2028 || cp == 0x2029 || (cp >= 0x202a && cp <= 0x202e) ||
        (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200e || cp == 0x200f || cp == 0x061c)
      fail("raw bidi / line-separator format character", out, i);
    i += len;
  }
}

static char const* hostile[] = {"\x1b]0;pwned\x07",
                                "\x1b[2J",
                                "\x1b[31m",
                                "\x1bP",
                                "\x1b\\",
                                "\x1b",
                                "\x07",
                                "\x08",
                                "\r",
                                "\x0c",
                                "\x00x",
                                "\x7f",
                                "\xc2\x9b"
                                "31m",
                                "\xc2\x9d"
                                "0;t\xc2\x9c",
                                "\xc2\x85",
                                "\xc2\x80",
                                "\x9b",
                                "\x9d",
                                "\xe2\x80\xa8",
                                "\xe2\x80\xa9",
                                "\xe2\x80\xae",
                                "\xe2\x81\xa6",
                                "\xe2\x80\x8f",
                                "\xd8\x9c",
                                "\xef\xbb\xbf",
                                "\xe2\x80\x8b",
                                "\xe2\x80\x8d",
                                "\xcc\x81",
                                "\xcc\x81\xcc\x82\xcc\x83\xcc\x84",
                                "\xed\xa0\x80",
                                "\xc0\xaf",
                                "\xf4\x90\x80\x80",
                                "\xff",
                                "\xfe",
                                "\xc3",
                                "\xe2\x82",
                                "\xf0\x9f\x98",
                                "\xf0\x9f\x98\x80",
                                "\xe4\xb8\xad",
                                "\xef\xbc\xa1",
                                "\t",
                                "\t\t",
                                " ",
                                "  ",
                                "a",
                                "word",
                                "\xf3\xa0\x80\x81",
                                "\xef\xbf\xbe",
                                "\xf4\x8f\xbf\xbf",
                                "\xe2\x80\x80",
                                "\xc2\xad",
                                "\xe1\x85\x9f",
                                "\xef\xbf\xb9",
                                "\x1b[?1049l",
                                "\x1b[6n",
                                "\x1b[200~",
                                "\x1b"
                                "c",
                                "\x0e",
                                "\x0f",
                                "\x1a",
                                "\x18"};
static char const* syntaxy[] = {"/*",          "*/",
                                "//",          "\"",
                                "'",           "R\"x(",
                                ")x\"",        "#include <a>",
                                "int",         "{",
                                "}",           "(",
                                ")",           "[",
                                "]",           "<",
                                ">",           "# ",
                                "**",          "`",
                                "```",         "~~~",
                                "[t](u)",      "| a | b |",
                                "|---|---|",   "> ",
                                "- ",          "<<<<<<< a",
                                "=======",     ">>>>>>> b",
                                "@@ -1 +1 @@", "+",
                                "-",           "diff --git a b",
                                "<<EOF",       "EOF",
                                "$x",          "${",
                                "key: |",      "- a",
                                "\"\"\"",      "'''",
                                "[t]",         "[[a]]",
                                "k = 1",       "def f():",
                                "@d",          "true",
                                "null",        "0x1F",
                                "1e9",         ":",
                                ",",           "\\",
                                "\\\"",        "%",
                                "&a",          "*a",
                                "!!x",         "---",
                                "..."};

static std::string genLine(std::size_t maxBits) {
  std::string s;
  for (std::size_t n = R(maxBits + 1); n; --n) {
    switch (R(4)) {
    case 0:
      s += hostile[R(std::size(hostile))];
      break;
    case 1:
    case 2:
      s += syntaxy[R(std::size(syntaxy))];
      break;
    case 3:
      s += static_cast<char>(R(256));
      break;
    }
  }
  std::erase(s, '\n');
  return s;
}

static std::string genContent() {
  std::string s;
  std::size_t lines = R(6) == 0 ? R(3) : R(60);
  for (std::size_t i = 0; i < lines; ++i) {
    s += genLine(R(12) == 0 ? 400 : 10);
    s += R(15) == 0 ? "\r\n" : "\n";
  }
  if (R(3) == 0)
    s += genLine(10);
  return s;
}

static BufferPoint randPoint(Buffer const& b) {
  std::size_t l = R(b.lineCount());
  auto const& line = b.lineContent(l);
  return {l, clusterStart(line, R(line.size() + 1))};
}

static void renderRandom(Buffer const& b, bool wholeFile) {
  std::size_t passes = wholeFile ? 6 : 2;
  for (std::size_t pass = 0; pass < passes; ++pass) {
    RenderParams p;
    p.point = randPoint(b);
    p.wrap = R(2);
    std::size_t vl =
        R(3) ? p.point.line - R(std::min<std::size_t>(p.point.line + 1, 5)) : R(b.lineCount());
    p.viewFrom.line = RowIdx{vl};
    std::size_t rows = R(8) == 0 ? 1 + R(3) : 1 + R(60), cols = R(8) == 0 ? 1 + R(4) : 1 + R(200);
    if (p.wrap) {
      // A wrapped viewport starts on a visual-row boundary of its first line.
      auto rb = computeRowBounds(b.lineContent(vl), cols);
      p.viewFrom.col = ByteOffset{rb[R(rb.size())]};
    } else {
      p.viewFrom.col = VisualCol{R(3) ? 0 : R(300)};
    }
    if (R(3) == 0)
      p.mark = randPoint(b);
    if (R(3) == 0) {
      auto a = randPoint(b), c = randPoint(b);
      p.highlight = Highlight{std::min(a, c), std::max(a, c)};
    }
    std::vector<BufferPoint> extra;
    if (R(4) == 0) {
      for (std::size_t n = 1 + R(5); n; --n)
        extra.push_back(randPoint(b));
      std::sort(extra.begin(), extra.end());
      extra.erase(std::unique(extra.begin(), extra.end()), extra.end());
      std::erase(extra, p.point);
      p.extraCursors = extra;
    }
    p.showWhitespace = R(3) == 0;
    p.focused = R(4) != 0;
    p.highlightCurrentLine = R(4) == 0;
    if (b.supportsLineNumberGutter() && R(3) == 0)
      p.leftGutter = std::to_string(b.lineCount()).size() + 1;
    p.rowOffset = R(3) ? 0 : R(50);
    p.colOffset = R(3) ? 0 : R(100);
    gCtx += " | rows=" + std::to_string(rows) + " cols=" + std::to_string(cols) +
            " wrap=" + std::to_string(p.wrap) + " ws=" + std::to_string(p.showWhitespace) +
            " gutter=" + std::to_string(p.leftGutter) + " viewLine=" + std::to_string(vl);
    std::string out;
    if (p.leftGutter >= cols)
      p.leftGutter = 0;
    auto [r, c] = b.render(out, RowIdx{rows}, ColIdx{cols}, p);
    checkTerminalSafe(out);
    (void)r;
    (void)c;
    (void)b.screenRow(ColIdx{cols}, p.point, p.viewFrom, p.wrap);
  }
}

int main(int argc, char** argv) {
  unsigned long long seed0 = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  unsigned long long iters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;
  std::signal(SIGABRT, onAbort);
  std::signal(SIGSEGV, onAbort);
  auto dir = std::filesystem::path(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") /
             ("render_fuzz" + std::to_string(getpid()));
  std::filesystem::create_directories(dir);
  char const* exts[] = {".txt", ".cpp",  ".md", ".json", ".diff", ".sh",
                        ".yml", ".toml", ".py", ".h",    ""};
  for (unsigned long long it = 0; it < iters; ++it) {
    unsigned long long seed = seed0 + it;
    rng.seed(seed);
    switch (R(10)) {
    default: { // text buffers in every mode, loaded from disk
      auto path = dir / (std::string("f") + exts[R(std::size(exts))]);
      std::ofstream(path, std::ios::binary) << genContent();
      gCtx = "seed " + std::to_string(seed) + " text " + path.filename().string();
      TextBuffer b(path.string(), path);
      renderRandom(b, true);
      // ...and after edits through a view (syntax cache invalidation paths)
      BufferView v(b);
      for (std::size_t n = R(6); n; --n) {
        auto pt = randPoint(b);
        v.setPoint(pt.line, pt.offset);
        if (R(2))
          v.insertText(genLine(3) + (R(3) ? "" : "\n"));
        else
          v.deleteChunkAtPoint();
        gCtx = "seed " + std::to_string(seed) + " text-after-edit " + path.filename().string();
        renderRandom(b, false);
      }
      break;
    }
    case 0: { // directory listing with hostile names
      auto d = dir / "d";
      std::error_code ec;
      std::filesystem::remove_all(d, ec);
      std::filesystem::create_directories(d);
      for (std::size_t n = R(8); n; --n) {
        std::string name = genLine(4);
        std::erase(name, '/');
        std::erase(name, '\0');
        if (name.empty() || name == "." || name == ".." || name.size() > 200)
          continue;
        if (R(3) == 0)
          std::filesystem::create_directory(d / name, ec);
        else
          std::ofstream(d / name) << "x";
      }
      gCtx = "seed " + std::to_string(seed) + " dir";
      DirBuffer b(d);
      renderRandom(b, true);
      break;
    }
    case 1: { // grep results with hostile paths and text
      std::vector<GrepHit> hits;
      for (std::size_t n = R(10); n; --n) {
        GrepHit h;
        auto display = genLine(3);
        h.file = std::make_shared<GrepFile const>(GrepFile{"/r/" + display, display});
        h.line = R(3) ? R(100000) : static_cast<std::size_t>(-1);
        h.text = genLine(R(5) == 0 ? 200 : 8);
        hits.push_back(std::move(h));
      }
      gCtx = "seed " + std::to_string(seed) + " grep";
      GrepBuffer b;
      b.refresh(genLine(3), "/r/" + genLine(2), std::move(hits), R(2) == 0);
      renderRandom(b, true);
      break;
    }
    case 2: { // buffer list with hostile names
      std::vector<std::string> names, dirs;
      for (std::size_t n = R(10); n; --n)
        names.push_back((R(2) ? "/" : "") + genLine(4));
      for (std::size_t n = R(4); n; --n)
        dirs.push_back("/" + genLine(3));
      gCtx = "seed " + std::to_string(seed) + " buffer-list";
      BufferListBuffer b(names, dirs, names.empty() ? std::string() : names[R(names.size())]);
      renderRandom(b, true);
      break;
    }
    case 3: { // paste ring with hostile clips
      PasteRing pr;
      for (std::size_t n = R(12); n; --n)
        pr.push(R(4) ? genLine(8) : genContent());
      gCtx = "seed " + std::to_string(seed) + " paste-ring";
      PasteRingBuffer b;
      b.refresh(pr);
      renderRandom(b, true);
      break;
    }
    }
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::printf("ok %llu iterations from seed %llu\n", iters, seed0);
}
