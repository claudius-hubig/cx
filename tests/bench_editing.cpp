// Benchmarks for editing, rendering, paste, syntax, and search performance.
//
// Usage:
//   ./build/bench_editing [--section NAME] [--lines N] [--iters N]
//
// Sections: edit, paste, render, syntax, search  (default: all)

#include "test_helpers.hpp"

#include "buffer/text_buffer.hpp"
#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;

struct Stats {
  double median, p95, p99, max;
};

static Stats computeStats(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto n = v.size();
  return {v[n / 2], v[n * 95 / 100], v[n * 99 / 100], v.back()};
}

static void printStats(char const* label, Stats const& s) {
  std::printf("  %-28s  median %7.0f us  p95 %7.0f us  p99 %7.0f us  max %7.0f us\n", label,
              s.median, s.p95, s.p99, s.max);
}

// Generate a file with N lines of ~lineLen characters, optionally looking
// like C++ code (for syntax highlighting benchmarks).
static std::string generateContent(std::size_t lines, std::size_t lineLen = 60,
                                   bool cppLike = false) {
  std::string content;
  content.reserve(lines * (lineLen + 1));
  for (std::size_t i = 0; i < lines; ++i) {
    if (cppLike) {
      std::string line = "  int var_" + std::to_string(i) + " = " + std::to_string(i * 17) +
                         "; // comment for line " + std::to_string(i);
      while (line.size() < lineLen)
        line += " ";
      line.resize(lineLen);
      content += line;
    } else {
      std::string prefix = "Line " + std::to_string(i) + ": ";
      std::string line = prefix;
      while (line.size() < lineLen)
        line += "abcdefghij";
      line.resize(lineLen);
      content += line;
    }
    content += '\n';
  }
  return content;
}

// =========================================================================
// 1. Edit vs file size: insert char, insert newline, delete region
// =========================================================================
static void benchEdit(std::size_t iters) {
  std::printf("=== Edit vs file size ===\n");

  for (std::size_t bufLines : {1000, 10000, 50000}) {
    auto content = generateContent(bufLines);
    std::size_t midLine = bufLines / 2;

    // --- Insert character at mid-file ---
    {
      std::vector<double> durations;
      durations.reserve(iters);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());
      BufferView view(buf);
      view.setPoint(midLine, 0);

      for (std::size_t i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        buf.insertAt('x', {midLine, 0});
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "insertChar  (%zuk lines)", bufLines / 1000);
      printStats(label, s);
    }

    // --- Insert newline at mid-file ---
    {
      std::vector<double> durations;
      durations.reserve(iters);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());

      for (std::size_t i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        buf.insertAt('\n', {midLine, 5});
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
        // Undo: delete the new line to keep buffer stable
        buf.deleteRegion({midLine, 5}, {midLine + 1, 0});
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "insertNewline (%zuk lines)", bufLines / 1000);
      printStats(label, s);
    }

    // --- Delete region (single line) at mid-file ---
    {
      std::vector<double> durations;
      durations.reserve(iters);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());

      for (std::size_t i = 0; i < iters; ++i) {
        // Insert then delete to measure delete cost
        buf.insertAt('x', {midLine, 0});
        auto t0 = Clock::now();
        buf.deleteRegion({midLine, 0}, {midLine, 1});
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "deleteRegion  (%zuk lines)", bufLines / 1000);
      printStats(label, s);
    }

    // --- Delete multi-line region at mid-file ---
    {
      std::vector<double> durations;
      durations.reserve(iters);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());

      for (std::size_t i = 0; i < iters; ++i) {
        // Insert 10 newlines then delete the region
        for (int j = 0; j < 10; ++j)
          buf.insertAt('\n', {midLine, 0});
        auto t0 = Clock::now();
        buf.deleteRegion({midLine, 0}, {midLine + 10, 0});
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "deleteMultiLine (%zuk lines)", bufLines / 1000);
      printStats(label, s);
    }
  }
}

// =========================================================================
// 2. Multi-line paste
// =========================================================================
static void benchPaste(std::size_t iters) {
  std::printf("\n=== Multi-line paste ===\n");

  // Generate paste payloads of different sizes
  auto paste100 = generateContent(100, 40);
  auto paste1000 = generateContent(1000, 40);

  for (std::size_t bufLines : {1000, 10000, 50000}) {
    auto content = generateContent(bufLines);
    std::size_t midLine = bufLines / 2;

    for (auto& [pasteLines, pasteContent] :
         {std::pair<std::size_t, std::string const&>{100, paste100},
          std::pair<std::size_t, std::string const&>{1000, paste1000}}) {
      std::vector<double> durations;
      durations.reserve(iters);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());

      for (std::size_t i = 0; i < iters; ++i) {
        auto t0 = Clock::now();
        auto end = buf.insertText(pasteContent, {midLine, 0});
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
        // Undo: delete pasted region
        buf.deleteRegion({midLine, 0}, end);
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "paste %4zu into %zuk", pasteLines, bufLines / 1000);
      printStats(label, s);
    }
  }
}

// =========================================================================
// 3. Render latency
// =========================================================================
static void benchRender(std::size_t iters) {
  std::printf("\n=== Render latency ===\n");

  struct TermSize {
    std::size_t rows, cols;
    char const* label;
  };
  TermSize sizes[] = {{24, 80, "80x24"}, {60, 200, "200x60"}};

  for (auto& [rows, cols, sizeLabel] : sizes) {
    for (std::size_t bufLines : {1000, 10000}) {
      auto content = generateContent(bufLines);
      TempFile f(content);
      TextBuffer buf(f.path().string(), f.path());
      BufferView view(buf);

      // Place cursor mid-file so viewport is in the middle
      std::size_t midLine = bufLines / 2;
      view.setPoint(midLine, 0);
      view.ensureVisible(RowIdx{rows}, ColIdx{cols}, false);

      std::string renderBuf;
      renderBuf.reserve(rows * cols * 2);
      std::vector<double> durations;
      durations.reserve(iters);

      for (std::size_t i = 0; i < iters; ++i) {
        renderBuf.clear();
        auto params = renderParams(view, false, false);
        auto t0 = Clock::now();
        buf.render(renderBuf, RowIdx{rows}, ColIdx{cols}, params);
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "%s txt %zuk lines", sizeLabel, bufLines / 1000);
      printStats(label, s);
    }
  }

  // Render with C++ syntax highlighting
  for (auto& [rows, cols, sizeLabel] : sizes) {
    auto content = generateContent(10000, 60, true);
    TempFile f(content, ".cpp");
    TextBuffer buf(f.path().string(), f.path());
    BufferView view(buf);

    view.setPoint(5000, 0);
    view.ensureVisible(RowIdx{rows}, ColIdx{cols}, false);

    std::string renderBuf;
    renderBuf.reserve(rows * cols * 2);
    std::vector<double> durations;
    durations.reserve(iters);

    for (std::size_t i = 0; i < iters; ++i) {
      renderBuf.clear();
      auto params = renderParams(view, false, false);
      auto t0 = Clock::now();
      buf.render(renderBuf, RowIdx{rows}, ColIdx{cols}, params);
      auto t1 = Clock::now();
      durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
    }
    auto s = computeStats(durations);
    char label[64];
    std::snprintf(label, sizeof(label), "%s cpp 10k lines", sizeLabel);
    printStats(label, s);
  }
}

// =========================================================================
// 4. Syntax re-tokenization: edit then render
// =========================================================================
static void benchSyntax(std::size_t iters) {
  std::printf("\n=== Syntax re-tokenization (edit + render) ===\n");

  std::size_t rows = 40, cols = 120;

  for (std::size_t bufLines : {1000, 10000}) {
    auto content = generateContent(bufLines, 60, true);
    TempFile f(content, ".cpp");
    TextBuffer buf(f.path().string(), f.path());
    BufferView view(buf);

    std::size_t midLine = bufLines / 2;
    view.setPoint(midLine, 0);
    view.ensureVisible(RowIdx{rows}, ColIdx{cols}, false);

    std::string renderBuf;
    renderBuf.reserve(rows * cols * 2);
    std::vector<double> durations;
    durations.reserve(iters);

    for (std::size_t i = 0; i < iters; ++i) {
      // Edit: insert a character (invalidates syntax cache from midLine onward)
      buf.insertAt('/', {midLine, 0});

      renderBuf.clear();
      auto params = renderParams(view, false, false);
      auto t0 = Clock::now();
      buf.render(renderBuf, RowIdx{rows}, ColIdx{cols}, params);
      auto t1 = Clock::now();
      durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));

      // Undo: remove inserted char to keep buffer stable
      buf.deleteRegion({midLine, 0}, {midLine, 1});
    }
    auto s = computeStats(durations);
    char label[64];
    std::snprintf(label, sizeof(label), "edit+render cpp %zuk lines", bufLines / 1000);
    printStats(label, s);
  }
}

// =========================================================================
// 5. Search performance
// =========================================================================
static void benchSearch(std::size_t iters) {
  std::printf("\n=== Search performance ===\n");

  for (std::size_t bufLines : {10000, 50000}) {
    auto content = generateContent(bufLines);
    TempFile f(content);
    TextBuffer buf(f.path().string(), f.path());

    // Short pattern (3 chars)
    {
      std::string pattern = "ine";
      FindFlags flags(pattern);

      std::vector<double> durations;
      durations.reserve(iters);

      for (std::size_t i = 0; i < iters; ++i) {
        std::size_t matches = 0;
        auto t0 = Clock::now();
        for (std::size_t line = 0; line < buf.lineCount(); ++line) {
          auto const& lineStr = buf.lineContent(line);
          std::size_t from = 0;
          while (auto hit = findFirstMatch(lineStr, from, pattern, flags)) {
            ++matches;
            from = hit->second;
          }
        }
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "search short  %zuk lines", bufLines / 1000);
      printStats(label, s);
    }

    // Long pattern (10 chars)
    {
      std::string pattern = "abcdefghij";
      FindFlags flags(pattern);

      std::vector<double> durations;
      durations.reserve(iters);

      for (std::size_t i = 0; i < iters; ++i) {
        std::size_t matches = 0;
        auto t0 = Clock::now();
        for (std::size_t line = 0; line < buf.lineCount(); ++line) {
          auto const& lineStr = buf.lineContent(line);
          std::size_t from = 0;
          while (auto hit = findFirstMatch(lineStr, from, pattern, flags)) {
            ++matches;
            from = hit->second;
          }
        }
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "search long   %zuk lines", bufLines / 1000);
      printStats(label, s);
    }

    // Case-insensitive pattern
    {
      std::string pattern = "line";
      FindFlags flags(pattern);

      std::vector<double> durations;
      durations.reserve(iters);

      for (std::size_t i = 0; i < iters; ++i) {
        std::size_t matches = 0;
        auto t0 = Clock::now();
        for (std::size_t line = 0; line < buf.lineCount(); ++line) {
          auto const& lineStr = buf.lineContent(line);
          std::size_t from = 0;
          while (auto hit = findFirstMatch(lineStr, from, pattern, flags)) {
            ++matches;
            from = hit->second;
          }
        }
        auto t1 = Clock::now();
        durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
      }
      auto s = computeStats(durations);
      char label[64];
      std::snprintf(label, sizeof(label), "search ci     %zuk lines", bufLines / 1000);
      printStats(label, s);
    }
  }
}

// =========================================================================

int main(int argc, char* argv[]) {
  std::string section;
  std::size_t iters = 200;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--section") == 0 && i + 1 < argc)
      section = argv[++i];
    else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
      iters = std::stoul(argv[++i]);
  }

  bool all = section.empty();

  if (all || section == "edit")
    benchEdit(iters);
  if (all || section == "paste")
    benchPaste(iters);
  if (all || section == "render")
    benchRender(iters);
  if (all || section == "syntax")
    benchSyntax(iters);
  if (all || section == "search")
    benchSearch(iters);
}
