// Fuzz BufferManager and DirBuffer against a private temp tree whose files
// change behind their back. All paths stay inside $TMPDIR/mgr_fuzz<pid>;
// a sentinel outside the working directory must survive every delete/rename.
// usage: mgr_fuzz SEED ITERS
#include "buffer/buffer_manager.hpp"
#include "buffer/buffer_sink.hpp"
#include "buffer/buffer_view.hpp"
#include "buffer/dir_buffer.hpp"
#include "buffer/text_buffer.hpp"
#include "paste_ring.hpp"

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

namespace fs = std::filesystem;
static std::mt19937_64 rng;
static std::size_t R(std::size_t n) { return n ? rng() % n : 0; }
static std::string last;
static void onAbort(int) {
  std::fprintf(stderr, "ABORT after: %s\n", last.c_str());
  void* bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
  _exit(134);
}
[[noreturn]] static void fail(std::string const& w) {
  std::fprintf(stderr, "FAIL: %s (after: %s)\n", w.c_str(), last.c_str());
  std::exit(3);
}

// Delete the tree at @p p. The fuzzer makes directories unreadable on
// purpose, and remove_all cannot delete what it cannot list, so every
// directory gets its owner permissions back first.
static void wipe(fs::path const& p) {
  std::error_code ec;
  if (fs::symlink_status(p, ec).type() == fs::file_type::directory) {
    fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec);
    std::vector<fs::path> children;
    for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec))
      children.push_back(it->path());
    for (auto const& child : children)
      wipe(child);
  }
  fs::remove_all(p, ec);
}

struct Sink : BufferSink {
  Buffer* cur = nullptr;
  void setBuffer(Buffer& b) override { cur = &b; }
};

static char const* names[] = {"a.txt",
                              "b.cpp",
                              "c.md",
                              "sub",
                              "sub/x.txt",
                              "sub/deep",
                              "sub/deep/y.json",
                              ".hidden",
                              "sp ace",
                              "nl\nname",
                              "\x1b[31mred",
                              "\xff\xfe",
                              "\xe4\xb8\xad.py",
                              "-dash",
                              "a:1",
                              "..",
                              ".",
                              "",
                              "../sentinel",
                              "sub/..",
                              "sub/../a.txt",
                              "./a.txt",
                              "loop",
                              "dangling",
                              "tab\tname",
                              "very"
                              "longlonglonglonglonglonglonglonglonglonglonglonglonglonglong.txt",
                              "\xc2\x9b"
                              "c1",
                              "a.txt/",
                              "sub/",
                              "//a.txt",
                              "a.txt/x",
                              "~"};

int main(int argc, char** argv) {
  unsigned long long seed0 = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  unsigned long long iters = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;
  std::signal(SIGABRT, onAbort);
  std::signal(SIGSEGV, onAbort);
  fs::path top = fs::path(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") /
                 ("mgr_fuzz" + std::to_string(getpid()));
  for (unsigned long long it = 0; it < iters; ++it) {
    rng.seed(seed0 + it);
    std::error_code ec;
    wipe(top);
    fs::path work = top / "outer" / "work";
    fs::create_directories(work / "sub" / "deep");
    std::ofstream(top / "outer" / "sentinel") << "keep";
    auto populate = [&] {
      for (auto n : {"a.txt", "b.cpp", "c.md", "sub/x.txt", "sub/deep/y.json", ".hidden", "sp ace",
                     "nl\nname", "\x1b[31mred", "\xff\xfe", "-dash", "a:1"})
        if (R(3))
          std::ofstream(work / n) << "line1\nline2 \xe4\xb8\xad\n";
      fs::create_symlink("loop", work / "loop", ec);
      fs::create_symlink("nowhere", work / "dangling", ec);
    };
    populate();
    fs::current_path(work);
    Sink sink;
    BufferManager mgr(sink);
    PasteRing pr;
    DirBuffer* dir = nullptr;
    auto rname = [&] { return std::string(names[R(std::size(names))]); };
    auto rpath = [&]() -> fs::path {
      auto n = rname();
      return R(3) ? fs::path(n) : work / n;
    };
    auto anyBuffer = [&]() -> std::string {
      auto v = mgr.bufferNames();
      return v.empty() ? std::string() : v[R(v.size())];
    };
    for (int op = 0; op < 120; ++op) {
      int k = static_cast<int>(R(24));
      last = "seed " + std::to_string(seed0 + it) + " op " + std::to_string(op) + " kind " +
             std::to_string(k);
      switch (k) {
      case 0:
      case 1:
      case 2: {
        auto p = rpath();
        last += " open " + p.string();
        auto& b = mgr.open(p);
        (void)b.lineCount();
        break;
      }
      case 3:
        mgr.create("*" + std::to_string(R(4)) + "*");
        break;
      case 4: {
        auto n = anyBuffer();
        if (!n.empty()) {
          last += " kill " + n;
          auto& b = mgr.kill(n);
          (void)b.name();
          dir = nullptr;
        }
        break;
      }
      case 5: {
        auto n = anyBuffer();
        auto p = rpath();
        last += " rename " + n + " -> " + p.string();
        if (!n.empty())
          (void)mgr.rename(n, p);
        break;
      }
      case 6:
        mgr.touch(R(2) ? anyBuffer() : rname());
        break;
      case 7:
        (void)mgr.secondMRU();
        break;
      case 8: {
        auto p = rname().substr(0, R(6));
        last += " completions " + p;
        (void)mgr.completions(R(2) ? p : (work / p).string());
        break;
      }
      case 9: {
        auto& bl = mgr.bufferListBuffer();
        std::string out;
        RenderParams rp;
        bl.render(out, RowIdx{10}, ColIdx{40}, rp);
        break;
      }
      case 10: {
        auto p = R(2) ? work : work / (R(2) ? "sub" : rname());
        last += " openDir " + p.string();
        // stay inside the tree: openDir on a path that resolves outside is not interesting here
        auto canon = fs::weakly_canonical(p, ec);
        if (!ec && canon.string().starts_with(work.string()))
          dir = &mgr.openDir(p);
        break;
      }
      case 11:
        if (dir) {
          auto p = BufferPoint{R(dir->lineCount() + 2), R(5)};
          (void)dir->selectedPath(p);
          (void)dir->selectedIsDir(p);
        }
        break;
      case 12:
        if (dir)
          dir->toggleDeleteMark({R(dir->lineCount() + 2), 0});
        break;
      case 13:
        if (dir && dir->directory().string().starts_with(work.string())) {
          last += " executeDelete in " + dir->directory().string();
          (void)dir->executeDelete();
        }
        break;
      case 14:
        if (dir) {
          auto n = rname();
          last += " renameEntry -> " + n;
          (void)dir->renameEntry({R(dir->lineCount() + 2), 0}, n);
        }
        break;
      case 15:
        if (dir) {
          auto n = rname();
          last += " makeSubdir " + n;
          (void)dir->makeSubdir(n);
        }
        break;
      case 16:
        if (dir)
          (void)dir->toggleHidden({R(dir->lineCount() + 1), 0});
        break;
      case 17:
        if (dir) {
          auto p = dir->selectedPath({R(dir->lineCount() + 1), 0});
          auto canon = fs::weakly_canonical(p, ec);
          if (!ec && canon.string().starts_with(work.string()) && fs::is_directory(canon, ec)) {
            last += " navigate " + p.string();
            (void)dir->navigate(p);
          }
        }
        break;
      case 18:
        if (dir) {
          std::string out;
          RenderParams rp;
          rp.point = {R(dir->lineCount()), 0};
          dir->render(out, RowIdx{1 + R(30)}, ColIdx{1 + R(80)}, rp);
        }
        break;
      case 19: // the tree changes behind the editor's back
        switch (R(6)) {
        case 0:
          fs::remove_all(work / "sub", ec);
          break;
        case 1:
          populate();
          fs::create_directories(work / "sub" / "deep", ec);
          break;
        case 2: {
          auto n = rname();
          if (n.find("..") == std::string::npos)
            fs::remove(work / n, ec);
          break;
        }
        case 3:
          fs::permissions(work / "sub", R(2) ? fs::perms::none : fs::perms::owner_all, ec);
          break;
        case 4: {
          std::ofstream f(work / "a.txt", std::ios::binary);
          for (int i = 0; i < 2000; ++i)
            f << "changed " << i << "\n";
          break;
        }
        case 5:
          fs::remove(work / "a.txt", ec);
          fs::create_directory(work / "a.txt", ec);
          break;
        }
        break;
      case 20: {
        auto n = anyBuffer();
        if (auto* b = mgr.find(n)) {
          last += " edit/save/revert " + n;
          BufferView v(*b);
          v.insertText("x\ny");
          (void)b->save();
          (void)b->revert();
          std::string out;
          v.render(out, RowIdx{5}, ColIdx{20});
        }
        break;
      }
      case 21:
        (void)mgr.pasteRingBuffer(pr);
        pr.push(rname());
        break;
      case 22: {
        std::vector<GrepHit> hits;
        for (std::size_t n = R(4); n; --n) {
          auto display = rname();
          hits.push_back(
              {std::make_shared<GrepFile const>(GrepFile{work / display, display}), R(9), rname()});
        }
        auto& g = mgr.grepBuffer(rname(), work, std::move(hits), R(2) == 0);
        (void)g.selectedHit({R(g.lineCount() + 1), 0});
        break;
      }
      case 23:
        (void)mgr.displayName(anyBuffer());
        (void)mgr.helpBuffer().lineCount();
        break;
      }
      // invariants
      auto all = mgr.bufferNames();
      for (auto const& n : all)
        if (!mgr.find(n))
          fail("bufferNames() lists '" + n + "' but find() does not know it");
      if (!fs::exists(top / "outer" / "sentinel"))
        fail("a file outside the working directory was removed or renamed");
      if (dir && dir->lineCount() == 0)
        fail("dir buffer with zero lines");
    }
    fs::permissions(work / "sub", fs::perms::owner_all, ec);
  }
  std::error_code ec;
  fs::current_path("/", ec);
  wipe(top);
  std::printf("ok %llu iterations from seed %llu\n", iters, seed0);
}
