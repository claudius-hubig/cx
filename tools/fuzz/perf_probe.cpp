// Timing probes on large / pathological files (release library).
// Each probe runs in a forked child with a timeout; anything slower than
// SLOW_MS is flagged. usage: perf_probe [SLOW_MS]
#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "regex_search.hpp"
#include "replace.hpp"
#include "search.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static double slowMs = 300;
static std::filesystem::path dir;

static double ms(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Probe {
  char const* name;
  std::function<void(BufferView&, TextBuffer&)> fn;
};

static std::string makeFile(char const* kind) {
  std::string k = kind, s;
  if (k == "1M short lines") {
    for (int i = 0; i < 1000000; ++i)
      s += "line " + std::to_string(i) + " some text here\n";
  } else if (k == "one 20MB line") {
    s.assign(20u << 20, 'x');
  } else if (k == "one 20MB line of words") {
    while (s.size() < (20u << 20))
      s += "word ";
  } else if (k == "one 5MB line of CJK") {
    while (s.size() < (5u << 20))
      s += "\xe4\xb8\xad";
  } else if (k == "one 5MB line of invalid bytes") {
    s.assign(5u << 20, '\xff');
  } else if (k == "one 5MB line of combining marks") {
    s = "e";
    while (s.size() < (5u << 20))
      s += "\xcc\x81";
  } else if (k == "one 5MB line of tabs") {
    s.assign(5u << 20, '\t');
  } else if (k == "200k-line C file, unclosed comment") {
    s = "/* never closed\n";
    for (int i = 0; i < 200000; ++i)
      s += "int f" + std::to_string(i) + "(void) { return (a[" + std::to_string(i) + "] << 2); }\n";
  } else if (k == "200k open brackets (cpp)") {
    for (int i = 0; i < 200000; ++i)
      s += "{(\n";
  } else if (k == "300k-row markdown table") {
    s = "| a | b | c |\n|---|---|---|\n";
    for (int i = 0; i < 300000; ++i)
      s += "| x" + std::to_string(i) + " | some words to wrap here | z |\n";
  } else if (k == "400k one-word paragraphs lines (md)") {
    for (int i = 0; i < 400000; ++i)
      s += "word" + std::to_string(i % 7) + " and more words in the paragraph\n";
  } else if (k == "10MB of NUL bytes") {
    s.assign(10u << 20, '\0');
  } else if (k == "2M empty lines") {
    s.assign(2000000, '\n');
  } else if (k == "CRLF 500k lines") {
    for (int i = 0; i < 500000; ++i)
      s += "crlf line\r\n";
  } else if (k == "deep heredoc / yaml block (sh)") {
    s = "cat <<EOF\n";
    for (int i = 0; i < 300000; ++i)
      s += "  body $X ${Y} `z`\n";
  }
  return s;
}

int main(int argc, char** argv) {
  if (argc > 1)
    slowMs = std::atof(argv[1]);
  dir =
      std::filesystem::path(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") / "perf_probe";
  std::filesystem::create_directories(dir);
  struct F {
    char const* kind;
    char const* ext;
  } files[] = {{"1M short lines", ".txt"},
               {"one 20MB line", ".txt"},
               {"one 20MB line of words", ".txt"},
               {"one 5MB line of CJK", ".txt"},
               {"one 5MB line of invalid bytes", ".txt"},
               {"one 5MB line of combining marks", ".txt"},
               {"one 5MB line of tabs", ".txt"},
               {"200k-line C file, unclosed comment", ".cpp"},
               {"200k open brackets (cpp)", ".cpp"},
               {"300k-row markdown table", ".md"},
               {"400k one-word paragraphs lines (md)", ".md"},
               {"10MB of NUL bytes", ".txt"},
               {"2M empty lines", ".txt"},
               {"CRLF 500k lines", ".txt"},
               {"deep heredoc / yaml block (sh)", ".sh"}};
  RowIdx rows{50};
  ColIdx cols{120};
  auto draw = [&](BufferView& v) {
    v.ensureVisible(rows, cols, v.wrap());
    std::string out;
    v.render(out, rows, cols);
  };
  std::vector<Probe> probes = {
      {"first draw", [&](BufferView& v, TextBuffer&) { draw(v); }},
      {"goto last line + draw",
       [&](BufferView& v, TextBuffer& b) {
         v.gotoLine(b.lineCount(), rows);
         draw(v);
       }},
      {"End + draw (wrap on)",
       [&](BufferView& v, TextBuffer&) {
         v.moveEnd();
         draw(v);
       }},
      {"End + draw (wrap off)",
       [&](BufferView& v, TextBuffer&) {
         v.setWrap(false);
         v.moveEnd();
         draw(v);
       }},
      {"10 x PageDown + draw",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 10; ++i) {
           v.pageDown(rows, cols, v.wrap());
           draw(v);
         }
       }},
      {"10 x Down from mid-file + draw",
       [&](BufferView& v, TextBuffer& b) {
         v.gotoLine(b.lineCount() / 2, rows);
         draw(v);
         for (int i = 0; i < 10; ++i) {
           v.moveDown(cols);
           draw(v);
         }
       }},
      {"type 10 chars at start + draw",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 10; ++i) {
           v.insert('a');
           draw(v);
         }
       }},
      {"type 10 chars at end of file + draw",
       [&](BufferView& v, TextBuffer& b) {
         v.gotoLine(b.lineCount(), rows);
         v.moveEnd();
         for (int i = 0; i < 10; ++i) {
           v.insert('a');
           draw(v);
         }
       }},
      {"10 x Enter mid-file + draw",
       [&](BufferView& v, TextBuffer& b) {
         v.gotoLine(b.lineCount() / 2, rows);
         for (int i = 0; i < 10; ++i) {
           v.insert('\n');
           draw(v);
         }
       }},
      {"word-right x 50",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 50; ++i)
           v.moveWordRight();
         draw(v);
       }},
      {"chunk delete forward",
       [&](BufferView& v, TextBuffer&) {
         v.deleteChunkAtPoint();
         draw(v);
       }},
      {"delete line x 10",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 10; ++i)
           v.deleteLine();
         draw(v);
       }},
      {"reflow (Alt-q, md tables)",
       [&](BufferView& v, TextBuffer&) {
         v.reflowParagraph(80, BufferView::ReflowTables::Markdown);
         draw(v);
       }},
      {"select all + uppercase",
       [&](BufferView& v, TextBuffer& b) {
         v.toggleMark();
         v.gotoLine(b.lineCount(), rows);
         v.moveEnd();
         v.convertCase(BufferView::CaseOp::Upper);
         draw(v);
       }},
      {"select all + toggle comment",
       [&](BufferView& v, TextBuffer& b) {
         v.toggleMark();
         v.gotoLine(b.lineCount(), rows);
         v.moveEnd();
         v.toggleComment("// ");
         draw(v);
       }},
      {"select all + cut",
       [&](BufferView& v, TextBuffer& b) {
         v.toggleMark();
         auto m = *v.mark();
         v.gotoLine(b.lineCount(), rows);
         v.moveEnd();
         auto t = v.extractRegion(m, v.point());
         v.deleteRegion(m, v.point());
         draw(v);
       }},
      {"cursor at every occurrence + type",
       [&](BufferView& v, TextBuffer&) {
         v.moveWordRight();
         v.moveWordLeft();
         v.selectAllOccurrences();
         v.insert('q');
         draw(v);
       }},
      {"matching bracket",
       [&](BufferView& v, TextBuffer&) {
         v.gotoMatchingBracket();
         draw(v);
       }},
      {"paragraph down x 20",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 20; ++i)
           v.moveParagraphDown();
         draw(v);
       }},
      {"find 'zzz' (no match) forward",
       [&](BufferView&, TextBuffer& b) { (void)findNextMatchInBuffer(b, {0, 0}, "zzz"); }},
      {"smart find 'zzz' per line",
       [&](BufferView&, TextBuffer& b) {
         FindFlags f("zzz");
         for (std::size_t i = 0; i < b.lineCount(); ++i)
           (void)findFirstMatch(b.lineContent(i), 0, "zzz", f);
       }},
      {"regex 'z{3}' find (no match)",
       [&](BufferView&, TextBuffer& b) {
         std::string e;
         auto re = compileRegex("z{3}", e);
         (void)findNextRegexInBuffer(b, {0, 0}, *re);
       }},
      {"replace-all 'e' -> 'E'",
       [&](BufferView&, TextBuffer& b) { (void)replaceAllForward(b, {0, 0}, "e", "E"); }},
      {"undo browse: 5 edits, step back 5 + draw",
       [&](BufferView& v, TextBuffer&) {
         for (int i = 0; i < 5; ++i)
           v.insert('u');
         v.enterHistoryMode();
         for (int i = 0; i < 5; ++i) {
           v.stepHistory(-1);
           draw(v);
         }
         v.exitHistoryModeCommit();
       }},
      {"save", [&](BufferView&, TextBuffer& b) { (void)b.save(); }},
  };
  int slow = 0;
  for (auto const& f : files) {
    auto path = dir / (std::string("f") + f.ext);
    {
      auto content = makeFile(f.kind);
      std::ofstream(path, std::ios::binary) << content;
    }
    std::printf("== %s\n", f.kind);
    std::fflush(stdout);
    for (auto const& p : probes) {
      int pfd[2];
      if (pipe(pfd) != 0)
        return 1;
      pid_t pid = fork();
      if (pid == 0) {
        close(pfd[0]);
        alarm(30);
        auto t0 = std::chrono::steady_clock::now();
        TextBuffer b(path.string(), path);
        BufferView v(b);
        double loadMs = ms(t0);
        auto t1 = std::chrono::steady_clock::now();
        p.fn(v, b);
        double opMs = ms(t1);
        rusage ru{};
        getrusage(RUSAGE_SELF, &ru);
        double out[3] = {loadMs, opMs, ru.ru_maxrss / 1024.0};
        if (write(pfd[1], out, sizeof out) != sizeof out)
          _exit(1);
        _exit(0);
      }
      close(pfd[1]);
      double out[3] = {0, 0, 0};
      auto n = read(pfd[0], out, sizeof out);
      close(pfd[0]);
      int st = 0;
      waitpid(pid, &st, 0);
      if (WIFSIGNALED(st)) {
        std::printf("   %-42s %s (signal %d)\n", p.name,
                    WTERMSIG(st) == SIGALRM ? "TIMEOUT > 30 s" : "CRASH", WTERMSIG(st));
        ++slow;
      } else if (n != sizeof out || WEXITSTATUS(st) != 0) {
        std::printf("   %-42s FAILED (exit %d)\n", p.name, WEXITSTATUS(st));
        ++slow;
      } else if (out[1] > slowMs || out[2] > 1500) {
        std::printf("   %-42s %8.0f ms   (load %.0f ms, peak RSS %.0f MB)%s\n", p.name, out[1],
                    out[0], out[2], out[1] > slowMs ? "  SLOW" : "  BIG");
        ++slow;
      } else if (&p == &probes[0]) {
        std::printf("   %-42s %8.0f ms   (load %.0f ms, peak RSS %.0f MB)\n", p.name, out[1],
                    out[0], out[2]);
      }
      std::fflush(stdout);
    }
  }
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::printf("%d slow/failed probes (threshold %.0f ms)\n", slow, slowMs);
}
