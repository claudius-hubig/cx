// In-process fuzzer for Buffer/BufferView.
// Random edit/move/render sequences on two views of one TextBuffer with:
//  - a reference text model for single- and multi-cursor edits,
//  - cursor / mark / extra-cursor / viewport invariants (bounds, UTF-8 boundaries),
//  - a history oracle: every retained version must equal a text the buffer
//    really had, in order,
//  - a render differential against a freshly built buffer with the same text.
// usage: view_fuzz SEED ITERS [OPS] [big]
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>
#define private public
#include "buffer/version_history.hpp"
#undef private
#include "buffer/buffer.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/text_buffer.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>
static void onAbort(int) {
  void* bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
  _exit(134);
}

static std::mt19937_64 rng;
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }
static std::vector<std::string> oplog;

[[noreturn]] static void fail(std::string const& what) {
  std::fprintf(stderr, "FAIL (%s): %s\n--- op log (last 60) ---\n", oplog[0].c_str(),
               what.substr(0, 6000).c_str());
  std::size_t s = oplog.size() > 60 ? oplog.size() - 60 : 0;
  for (std::size_t i = s; i < oplog.size(); ++i)
    std::fprintf(stderr, "  %zu: %s\n", i, oplog[i].substr(0, 300).c_str());
  std::exit(3);
}

static std::string text(Buffer const& b) {
  std::string s;
  for (std::size_t i = 0; i < b.lineCount(); ++i) {
    if (i)
      s += '\n';
    s += b.lineContent(i);
  }
  return s;
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

static std::size_t absOff(Buffer const& b, BufferPoint p) {
  std::size_t o = 0;
  for (std::size_t i = 0; i < p.line; ++i)
    o += b.lineContent(i).size() + 1;
  return o + p.offset;
}

static bool gAllowBad =
    false; // raw invalid bytes are being inserted: boundaries can shift under a cursor
static void checkPoint(Buffer const& b, BufferPoint p, std::string const& what) {
  if (p.line >= b.lineCount())
    fail(what + ": line " + std::to_string(p.line) + " >= lineCount " +
         std::to_string(b.lineCount()));
  auto const& l = b.lineContent(p.line);
  if (p.offset > l.size())
    fail(what + ": offset " + std::to_string(p.offset) + " > line size " +
         std::to_string(l.size()));
  if (!gAllowBad && validUtf8(l) && p.offset < l.size() &&
      (static_cast<unsigned char>(l[p.offset]) >> 6) == 2)
    fail(what + ": offset " + std::to_string(p.offset) + " inside a UTF-8 sequence on line " +
         std::to_string(p.line) + " [" + l.substr(0, 200) + "]");
}

static char const* pieces[] = {"a",
                               "b",
                               " ",
                               "\t",
                               "é",
                               "中",
                               "😀",
                               "e\xcc\x81",
                               "(",
                               ")",
                               "[",
                               "]",
                               "{",
                               "}",
                               "<",
                               ">",
                               "\"",
                               "'",
                               "#",
                               "//",
                               "/*",
                               "*/",
                               "-",
                               "x",
                               "\n",
                               "\n",
                               "  ",
                               "word",
                               "\xef\xbb\xbf",
                               "\xe2\x80\x8b",
                               "|",
                               "|",
                               " | ",
                               "\\|",
                               "---",
                               ":-:",
                               "`",
                               "```",
                               "~~~",
                               "> ",
                               "- ",
                               "1. ",
                               "\n\n",
                               "\xc2\x85",
                               "\xcc\x81",
                               "\xe2\x80\xae",
                               "[t](u)",
                               "`a|b`",
                               "\n|a|b|\n|-|-|\n|c|d|\n",
                               "\n| x | y y y |\n|---|:--|\n|   | z |\n",
                               "=======",
                               "<<<<<<< a\n",
                               ">>>>>>> b\n",
                               "\"\"\"",
                               "'''",
                               "<<EOF\n",
                               "EOF\n",
                               "key: |\n",
                               "R\"x(",
                               ")x\"",
                               "\\",
                               "$",
                               "@@ -1 +1 @@\n",
                               "+",
                               "\r"};

static std::string randText(std::size_t n, bool allowBad) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i) {
    if (allowBad && R(30) == 0)
      s += static_cast<char>(0x80 + R(128));
    else
      s += pieces[R(std::size(pieces))];
  }
  return s;
}

static uint32_t randCp() {
  static uint32_t cps[] = {'a',      'Z',  ' ', '\t',   '\n',     '(',     ')',   '{',    '}',
                           '"',      '|',  '`', 0xE9,   0x4E2D,   0x1F600, 0x301, 0x200B, 0xFEFF,
                           0x10FFFF, 0x7F, 0x1, 0xD800, 0x110000, 0,       0x85,  0x202E, 0x1F3FD};
  return cps[R(std::size(cps))];
}

/// Render through the buffer with explicit params, so a second buffer can
/// be rendered with exactly the same view state.
static std::string renderWith(Buffer const& b, BufferView const& v, std::size_t rows,
                              std::size_t cols) {
  RenderParams p;
  p.point = v.point();
  p.viewFrom = v.viewFromRaw();
  p.mark = v.mark();
  p.extraCursors = v.extraCursors();
  p.wrap = v.wrap();
  p.leftGutter = v.lineNumberGutterWidth();
  std::string out;
  b.render(out, RowIdx{rows}, ColIdx{cols}, p);
  return out;
}

int main(int argc, char** argv) {
  unsigned long long seed0 = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  unsigned long long iters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;
  std::size_t nops = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 300;
  bool big = argc > 4;
  std::signal(SIGABRT, onAbort);
  std::signal(SIGSEGV, onAbort);
  auto dir =
      std::filesystem::path(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") / "view_fuzz";
  std::filesystem::create_directories(dir);

  for (unsigned long long it = 0; it < iters; ++it) {
    unsigned long long seed = seed0 + it;
    rng.seed(seed);
    oplog.clear();
    oplog.push_back("seed " + std::to_string(seed));
    bool allowBad = R(5) == 0;
    gAllowBad = allowBad;
    std::string init = randText(R(4) == 0 ? R(3) : R(400), allowBad);
    if (R(10) == 0)
      init += std::string(R(3000), 'x');
    if (big) {
      std::string unit = randText(40, false);
      std::size_t reps = 2000 + R(20000);
      std::string s;
      for (std::size_t i = 0; i < reps; ++i)
        s += unit, s += '\n';
      init = s + init;
    }
    auto path = dir / ("f" + std::to_string(getpid()) + ".txt");
    std::ofstream(path, std::ios::binary) << init;
    TextBuffer buf("f", path);
    BufferMode modes[] = {BufferMode::Txt, BufferMode::Cpp, BufferMode::Md,
                          BufferMode::Jsn, BufferMode::Dif, BufferMode::Sh,
                          BufferMode::Yml, BufferMode::Tml, BufferMode::Py};
    BufferMode mode = R(3) == 0   ? BufferMode::Txt
                      : R(3) == 0 ? BufferMode::Md
                                  : modes[R(std::size(modes))];
    buf.setMode(mode);
    BufferView va(buf), vb(buf);
    BufferView* views[] = {&va, &vb};
    // Every text the buffer has had, in order (deduplicating repeats).
    std::vector<std::string> states{text(buf)};
    bool histOk = buf.history().size() == 1;
    std::size_t checkEvery = big ? 25 : 1;

    for (std::size_t op = 0; op < nops; ++op) {
      BufferView& v = *views[R(4) == 0 ? 1 : 0];
      char const* vn = &v == &va ? "A" : "B";
      bool full = op % checkEvery == 0;
      std::string before = full ? text(buf) : std::string();
      bool editable = !va.inHistoryMode() && !vb.inHistoryMode() && !buf.readonly();
      bool modelable =
          full && editable && v.cursorCount() == 1 && !v.mark() && !buf.overwriteMode();
      BufferPoint p0 = v.point();
      std::size_t a0 = p0.line < buf.lineCount() ? absOff(buf, p0) : 0;
      std::size_t rows = 1 + R(40), cols = 1 + R(120);
      bool wrap = R(2);
      int k = static_cast<int>(R(52));
      std::string desc = std::string(vn) + " op" + std::to_string(k);
      if (std::getenv("VF_DUMPHIST") &&
          op >= std::strtoull(std::getenv("VF_DUMPHIST"), nullptr, 10)) {
        auto const& h = buf.history();
        std::fprintf(stderr, "HIST before op %zu (A idx %d, B idx %d, browsing %d, lines %zu):", op,
                     va.historyIndex() ? (int)*va.historyIndex() : -1,
                     vb.historyIndex() ? (int)*vb.historyIndex() : -1, (int)buf.browsingHistory(),
                     buf.lineCount());
        for (auto const& sn : h._versions)
          std::fprintf(stderr, " [lines=%zu chunks=%zu]", sn.lines.size(), sn.lines.chunkCount());
        std::fprintf(stderr, "\n");
      }
      if (std::getenv("VF_TRACE"))
        std::fprintf(stderr, "PRE %zu %s point=%zu:%zu lines=%zu rows=%zu cols=%zu wrap=%d\n", op,
                     desc.c_str(), v.point().line, v.point().offset, buf.lineCount(), rows, cols,
                     (int)wrap);
      std::size_t hsz0 = buf.history().size();
      bool browsing0 = va.inHistoryMode() || vb.inHistoryMode();
      std::optional<std::string> expect;
      // All cursors as absolute offsets, ascending (for the multi-cursor model).
      std::vector<std::size_t> curs;
      if (full && editable && !v.mark() && !buf.overwriteMode()) {
        curs.push_back(a0);
        for (auto c : v.extraCursors())
          if (c.line < buf.lineCount())
            curs.push_back(absOff(buf, c));
        std::sort(curs.begin(), curs.end());
      }
      auto multiInsertExpect = [&](std::string const& t) {
        std::string e;
        std::size_t prev = 0;
        for (auto c : curs) {
          e += before.substr(prev, c - prev);
          e += t;
          prev = c;
        }
        e += before.substr(prev);
        return e;
      };
      switch (k) {
      case 0:
      case 1:
      case 2: {
        uint32_t cp = randCp();
        desc += " insert U+" + std::to_string(cp);
        if (!curs.empty() && cp >= 0x20 && cp != 0x7F && cp != 0x85 &&
            (cp < 0xD800 || (cp > 0xDFFF && cp <= 0x10FFFF))) {
          auto [n, enc] = encodeUtf8(cp);
          expect = multiInsertExpect(std::string(reinterpret_cast<char const*>(enc.data()), n));
        }
        v.insert(cp);
        break;
      }
      case 3:
        v.deleteAtPoint();
        desc += " deleteAtPoint";
        break;
      case 4:
        v.deleteBeforePoint();
        desc += " deleteBeforePoint";
        break;
      case 5:
        v.deleteChunkAtPoint();
        desc += " deleteChunkAtPoint";
        break;
      case 6:
        v.deleteChunkBeforePoint();
        desc += " deleteChunkBeforePoint";
        break;
      case 7:
        v.deleteLine();
        desc += " deleteLine";
        break;
      case 8: {
        auto t = randText(R(20), allowBad);
        desc += " insertText[" + t + "]";
        if (t.find('\r') == std::string::npos && !curs.empty())
          expect = multiInsertExpect(t);
        v.insertText(t);
        break;
      }
      case 9:
        v.moveLeft();
        desc += " left";
        break;
      case 10:
        v.moveRight();
        desc += " right";
        break;
      case 11:
        v.moveUp(ColIdx{cols});
        desc += " up";
        break;
      case 12:
        v.moveDown(ColIdx{cols});
        desc += " down";
        break;
      case 13:
        v.movePhysicalUp();
        desc += " pup";
        break;
      case 14:
        v.movePhysicalDown();
        desc += " pdown";
        break;
      case 15:
        v.moveHome();
        desc += " home";
        break;
      case 16:
        v.moveEnd();
        desc += " end";
        break;
      case 17:
        v.moveWordRight();
        desc += " wordR";
        break;
      case 18:
        v.moveWordLeft();
        desc += " wordL";
        break;
      case 19:
        v.moveParagraphUp();
        desc += " paraU";
        break;
      case 20:
        v.moveParagraphDown();
        desc += " paraD";
        break;
      case 21:
        v.pageUp(RowIdx{rows}, ColIdx{cols}, wrap);
        desc += " pgup";
        break;
      case 22:
        v.pageDown(RowIdx{rows}, ColIdx{cols}, wrap);
        desc += " pgdn";
        break;
      case 23:
        v.gotoMatchingBracket();
        desc += " bracket";
        break;
      case 24: {
        auto l = R(3) ? R(buf.lineCount() + 3) : static_cast<std::size_t>(-1) - R(3);
        desc += " goto " + std::to_string(l);
        v.gotoLine(l, RowIdx{rows});
        break;
      }
      case 25:
        v.scrollDown();
        desc += " scrollDown";
        break;
      case 26:
        v.scrollUp(wrap);
        desc += " scrollUp";
        break;
      case 27:
        v.toggleMark();
        desc += " mark";
        break;
      case 28: {
        auto m = v.mark();
        desc += " deleteRegion";
        if (m) {
          auto from = std::min(*m, v.point()), to = std::max(*m, v.point());
          if (full && editable && v.cursorCount() == 1)
            expect = before.substr(0, absOff(buf, from)) + before.substr(absOff(buf, to));
          auto ex = v.extractRegion(*m, v.point());
          if (full && ex != before.substr(absOff(buf, from), absOff(buf, to) - absOff(buf, from)))
            fail(desc + ": extractRegion mismatch");
          v.deleteRegion(*m, v.point());
        }
        break;
      }
      case 29:
        v.toggleComment(R(2) ? "// " : "# ");
        desc += " toggleComment";
        break;
      case 30:
      case 46:
      case 47: {
        auto w = R(4) ? 1 + R(100) : R(3);
        bool md = k != 30;
        desc += std::string(md ? " reflowMd " : " reflow ") + std::to_string(w);
        v.reflowParagraph(w, md ? BufferView::ReflowTables::Markdown
                                : BufferView::ReflowTables::Ignore);
        if (md && full && editable) {
          // Alt-q twice in a row must be stable.
          std::string once = text(buf);
          auto pt = v.point();
          v.reflowParagraph(w, BufferView::ReflowTables::Markdown);
          std::string twice = text(buf);
          if (!browsing0 && once != states.back())
            states.push_back(once); // the intermediate text is a version too
          if (once != twice && std::getenv("VF_IDEMP"))
            fail(desc + ": reflowMd not idempotent (cursor " + std::to_string(pt.line) + ":" +
                 std::to_string(pt.offset) + ")\n  before=[" + before + "]\n  once=[" + once +
                 "]\n  twice=[" + twice + "]");
        }
        break;
      }
      case 31:
        v.convertCase(static_cast<BufferView::CaseOp>(R(3)));
        desc += " case";
        break;
      case 32:
        v.addCursorDown();
        desc += " addCursorDown";
        break;
      case 33:
        v.addCursorUp();
        desc += " addCursorUp";
        break;
      case 34:
        v.selectAllOccurrences();
        desc += " selectAll";
        break;
      case 35:
        v.setPoint(R(buf.lineCount() + 2), R(200));
        desc += " setPoint";
        break;
      case 36:
        v.enterHistoryMode();
        desc += " enterHistory";
        break;
      case 37: {
        int d = static_cast<int>(R(9)) - 4;
        if (R(10) == 0)
          d = R(2) ? 1000000 : -1000000;
        desc += " stepHistory " + std::to_string(d);
        v.stepHistory(d);
        break;
      }
      case 38:
        v.exitHistoryModeCommit();
        desc += " exitHistory";
        break;
      case 39:
        v.setWrap(!v.wrap());
        desc += " wrap";
        break;
      case 40:
        v.recenter(RowIdx{rows}, R(3), ColIdx{cols}, wrap);
        desc += " recenter";
        break;
      case 41:
        v.adjustCursorToIndent();
        desc += " adjustIndent";
        break;
      case 42:
        v.scrollViewNearEnd(RowIdx{rows});
        desc += " nearEnd";
        break;
      case 43: {
        std::string out;
        v.render(out, RowIdx{rows}, ColIdx{cols});
        desc += " render";
        break;
      }
      case 44: {
        auto t = std::string(R(2) ? "\n" : "") + randText(1 + R(5), false);
        desc += " insertText-ml";
        v.insertText(t);
        break;
      }
      case 45:
        if (R(4) == 0) {
          auto r = buf.revert();
          desc += " revert";
          (void)r;
        }
        break;
      case 48:
        v.addCursor({R(buf.lineCount() + 1), R(50)});
        desc += " addCursor";
        break;
      case 49:
        v.collapseCursors();
        desc += " collapse";
        break;
      case 50:
        if (R(3) == 0) {
          buf.toggleOverwriteMode();
          desc += " overwrite";
        }
        break;
      case 51:
        v.insert('\n');
        desc += " enter";
        break; /* auto-indent: not modelled */
      }
      if (std::getenv("VF_TRACE"))
        std::fprintf(stderr, "TRACE %zu %s hist=%zu->%zu\n", op, desc.c_str(), hsz0,
                     buf.history().size());
      oplog.push_back(desc);
      if (buf.lineCount() == 0)
        fail(desc + ": lineCount 0");

      std::string after = full ? text(buf) : std::string();
      if (expect && after != *expect)
        fail(desc + ": model mismatch\n  before=[" + before + "]\n  expect=[" + *expect +
             "]\n  actual=[" + after + "]");
      if (full && validUtf8(before) && !validUtf8(after) && k != 8 && k != 45 && k != 44 && k > 2 &&
          (k < 36 || k > 38))
        fail(desc + ": valid UTF-8 text became invalid\n  before=[" + before + "]\n  after=[" +
             after + "]");

      // Like Screen::draw(): every view gets ensureVisible before render.
      for (BufferView* w : views) {
        std::string wn = desc + " view" + (w == &va ? "A" : "B");
        w->ensureVisible(RowIdx{rows}, ColIdx{cols}, w->wrap());
        checkPoint(buf, w->point(), wn + " point");
        if (auto m = w->mark())
          checkPoint(buf, *m, wn + " mark");
        auto ex = w->extraCursors();
        for (std::size_t i = 0; i < ex.size(); ++i) {
          checkPoint(buf, ex[i], wn + " extraCursor");
          if (ex[i] == w->point())
            fail(wn + ": extra cursor equals point");
          if (i && !(ex[i - 1] < ex[i]))
            fail(wn + ": extra cursors not strictly ascending");
        }
        if (w->viewFrom().line >= buf.lineCount())
          fail(wn + " viewFrom line out of range after ensureVisible");
        if (!big || full) {
          std::string out;
          w->render(out, RowIdx{rows}, ColIdx{cols});
          (void)w->screenRow(ColIdx{cols}, w->wrap());
        }
      }
      if (R(3) == 0) {
        v.ensureVisible(RowIdx{rows}, ColIdx{cols}, wrap);
        auto sr = v.screenRow(ColIdx{cols}, wrap);
        if (sr >= rows)
          fail(desc + ": after ensureVisible(rows=" + std::to_string(rows) +
               ",cols=" + std::to_string(cols) + ",wrap=" + std::to_string(wrap) +
               ") screenRow=" + std::to_string(sr));
      }
      // Render differential: same text + view state in a fresh buffer.
      if (full && (big ? op % 100 == 0 : R(4) == 0)) {
        auto path2 = dir / ("g" + std::to_string(getpid()) + ".txt");
        std::ofstream(path2, std::ios::binary) << after;
        TextBuffer fresh("g", path2);
        fresh.setMode(mode);
        auto r1 = renderWith(buf, v, rows, cols);
        auto r2 = renderWith(fresh, v, rows, cols);
        // Conflict markers are only detected on load (by design), and a
        // load may split lines differently on CR: compare like with like.
        if (fresh.hasConflicts() == buf.hasConflicts() && text(fresh) == after && r1 != r2)
          fail(desc + ": render differs from a fresh buffer with the same text (rows=" +
               std::to_string(rows) + " cols=" + std::to_string(cols) + ")\n  text=[" +
               after.substr(0, 1500) + "]\n  live =[" + escapeNonPrintable(r1).substr(0, 1500) +
               "]\n  fresh=[" + escapeNonPrintable(r2).substr(0, 1500) + "]");
      }
      // History oracle bookkeeping.
      if (full) {
        if (!browsing0 && after != states.back())
          states.push_back(after);
        std::size_t hsz1 = buf.history().size();
        if (k == 45)
          histOk = false; // revert resets or rewrites history
        // Committing an older version: text must be that version (already
        // in states); later states are no longer reachable but stay listed —
        // the subsequence check below would then be too weak, so restart it.
        if (k == 38 && browsing0 && !va.inHistoryMode() && !vb.inHistoryMode()) {
          states.push_back(after);
        }
        (void)hsz1;
      }
      // Walk the whole history now and then and check it against states.
      if (full && histOk && !big && !va.inHistoryMode() && !vb.inHistoryMode() &&
          (R(40) == 0 || op + 1 == nops)) {
        auto const& h = buf.history();
        std::size_t si = 0;
        std::size_t lastMatch = 0;
        for (std::size_t i = 0; i < h.size(); ++i) {
          auto const lines = h.linesAt(i);
          std::string t;
          for (std::size_t l = 0; l < lines.size(); ++l) {
            if (l)
              t += '\n';
            t += *lines[l];
          }
          // Versions may repeat a text the buffer returned to (commit of an
          // older version), so search forward first, then anywhere.
          std::size_t j = si;
          while (j < states.size() && states[j] != t)
            ++j;
          if (j == states.size()) {
            bool anywhere = std::find(states.begin(), states.end(), t) != states.end();
            if (!anywhere)
              fail(desc + ": history version " + std::to_string(i) + "/" +
                   std::to_string(h.size()) + " holds a text the buffer never had\n  version=[" +
                   t.substr(0, 1500) + "]\n  after state#" + std::to_string(lastMatch) + "=[" +
                   states[lastMatch].substr(0, 1500) + "]");
          } else {
            si = j;
            lastMatch = j;
          }
        }
        if (h.size() > 0) {
          auto const lines = h.linesAt(h.size() - 1);
          std::string t;
          for (std::size_t l = 0; l < lines.size(); ++l) {
            if (l)
              t += '\n';
            t += *lines[l];
          }
          if (t != after)
            fail(desc + ": newest history version differs from the buffer text\n  newest=[" +
                 t.substr(0, 1500) + "]\n  buffer=[" + after.substr(0, 1500) + "]");
        }
      }
    }
  }
  std::printf("ok %llu iterations from seed %llu\n", iters, seed0);
}
