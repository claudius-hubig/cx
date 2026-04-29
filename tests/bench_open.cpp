// Benchmarks for file-open latency at various sizes.
//
// Usage:
//   ./build/bench_open [--lines N] [--iters N]

#include "test_helpers.hpp"

#include "buffer/text_buffer.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
using ms = std::chrono::microseconds;

struct Stats {
  double median, p95, max;
};

static Stats computeStats(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto n = v.size();
  return {v[n / 2], v[n * 95 / 100], v.back()};
}

static void printStats(char const* label, Stats const& s) {
  std::printf("  %-36s  median %8.0f us  p95 %8.0f us  max %8.0f us\n", label, s.median, s.p95,
              s.max);
}

static std::string generateContent(std::size_t lines, std::size_t lineLen = 60) {
  std::string content;
  content.reserve(lines * (lineLen + 1));
  for (std::size_t i = 0; i < lines; ++i) {
    std::string prefix = "Line " + std::to_string(i) + ": ";
    std::string line = prefix;
    while (line.size() < lineLen)
      line += "abcdefghij";
    line.resize(lineLen);
    content += line;
    content += '\n';
  }
  return content;
}

static void benchOpen(std::size_t maxLines, std::size_t iters) {
  std::printf("=== File open latency ===\n");

  std::vector<std::size_t> sizes = {1000, 10000, 50000, 250000};
  while (!sizes.empty() && sizes.back() > maxLines)
    sizes.pop_back();
  if (sizes.empty())
    sizes.push_back(maxLines);

  for (auto lines : sizes) {
    auto content = generateContent(lines);
    TempFile f(content);

    std::vector<double> durations;
    durations.reserve(iters);

    for (std::size_t i = 0; i < iters; ++i) {
      auto t0 = Clock::now();
      TextBuffer buf(f.path().string(), f.path());
      auto t1 = Clock::now();
      durations.push_back(static_cast<double>(std::chrono::duration_cast<ms>(t1 - t0).count()));
    }
    auto s = computeStats(durations);
    char label[64];
    std::snprintf(label, sizeof(label), "open %zuk lines", lines / 1000);
    printStats(label, s);
  }
}

static void benchOpenCpp(std::size_t maxLines, std::size_t iters) {
  std::printf("\n=== File open latency (C++ syntax) ===\n");

  std::vector<std::size_t> sizes = {10000, 50000, 250000};
  while (!sizes.empty() && sizes.back() > maxLines)
    sizes.pop_back();

  for (auto lines : sizes) {
    std::string content;
    content.reserve(lines * 70);
    for (std::size_t i = 0; i < lines; ++i) {
      content += "  int var_" + std::to_string(i) + " = " + std::to_string(i * 17) +
                 "; // comment for line " + std::to_string(i) + "\n";
    }
    TempFile f(content, ".cpp");

    std::vector<double> durations;
    durations.reserve(iters);

    for (std::size_t i = 0; i < iters; ++i) {
      auto t0 = Clock::now();
      TextBuffer buf(f.path().string(), f.path());
      auto t1 = Clock::now();
      durations.push_back(static_cast<double>(std::chrono::duration_cast<ms>(t1 - t0).count()));
    }
    auto s = computeStats(durations);
    char label[64];
    std::snprintf(label, sizeof(label), "open cpp %zuk lines", lines / 1000);
    printStats(label, s);
  }
}

int main(int argc, char* argv[]) {
  std::size_t lines = 250000;
  std::size_t iters = 10;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--lines") == 0 && i + 1 < argc)
      lines = std::stoul(argv[++i]);
    else if (std::strcmp(argv[i], "--iters") == 0 && i + 1 < argc)
      iters = std::stoul(argv[++i]);
  }

  benchOpen(lines, iters);
  benchOpenCpp(lines, iters);
}
