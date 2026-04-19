// Benchmark for version-history capture/prune performance.
//
// Two modes:
//   --direct   Exercises BufferView::insert() → capture() → prune() in-process
//   --pty      Measures per-keystroke latency through the real editor via PTY
//
// Both default to a 500-line buffer with 300 inserts.

#include "test_helpers.hpp"

#include "buffer/text_buffer.hpp"
#include "buffer/version_history.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
using us = std::chrono::microseconds;

static std::string generateContent(std::size_t lines, std::size_t lineLen = 60) {
  std::string content;
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

struct Stats {
  double median, p95, p99, max;
};

static Stats computeStats(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto n = v.size();
  return {v[n / 2], v[n * 95 / 100], v[n * 99 / 100], v.back()};
}

// ---------------------------------------------------------------------------
// Direct benchmark
// ---------------------------------------------------------------------------

static void benchDirect(std::size_t bufLines, std::size_t inserts) {
  auto content = generateContent(bufLines);
  TempFile f(content);
  TextBuffer buf(f.path().string(), f.path());
  BufferView view(buf);

  std::vector<double> durations;
  durations.reserve(inserts);

  for (std::size_t i = 0; i < inserts; ++i) {
    auto t0 = Clock::now();
    view.insert('x');
    auto t1 = Clock::now();
    durations.push_back(static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count()));
  }

  // Build indexed pairs BEFORE sorting for stats.
  std::vector<std::pair<double, std::size_t>> indexed;
  indexed.reserve(inserts);
  for (std::size_t i = 0; i < inserts; ++i)
    indexed.push_back({durations[i], i});

  auto stats = computeStats(durations);
  std::printf("Direct benchmark (%zu-line buffer, %zu inserts):\n", bufLines, inserts);
  std::printf("  median: %6.0f us\n", stats.median);
  std::printf("  p95:    %6.0f us\n", stats.p95);
  std::printf("  p99:    %6.0f us\n", stats.p99);
  std::printf("  max:    %6.0f us\n", stats.max);

  std::sort(indexed.begin(), indexed.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::printf("  slowest 10:\n");
  for (int j = 0; j < 10 && j < static_cast<int>(indexed.size()); ++j)
    std::printf("    insert #%3zu: %6.0f us  (history ~%zu)\n", indexed[j].second, indexed[j].first,
                std::min(indexed[j].second + 1, (std::size_t)VersionHistory::kMaxVersions));
}

// ---------------------------------------------------------------------------
// PTY benchmark
// ---------------------------------------------------------------------------

struct Pty {
  int fd = -1;
  pid_t pid = -1;

  Pty(std::size_t rows, std::size_t cols, std::string const& binary,
      std::vector<std::string> const& args = {}) {
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    pid = forkpty(&fd, nullptr, nullptr, &ws);
    if (pid == 0) {
      std::vector<char*> argv;
      std::string binCopy = binary;
      argv.push_back(binCopy.data());
      std::vector<std::string> ac(args.begin(), args.end());
      for (auto& a : ac)
        argv.push_back(a.data());
      argv.push_back(nullptr);
      execv(binary.c_str(), argv.data());
      _exit(127);
    }
  }

  ~Pty() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      int st = 0;
      waitpid(pid, &st, 0);
    }
    if (fd >= 0)
      close(fd);
  }

  Pty(Pty const&) = delete;
  Pty& operator=(Pty const&) = delete;

  void send(char c) { ::write(fd, &c, 1); }

  void drain(int timeoutMs) {
    char buf[4096];
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN)) {
      if (::read(fd, buf, sizeof(buf)) <= 0)
        break;
    }
  }

  // Wait until at least one byte of output is available.
  // Returns the wait time in microseconds, or -1 on timeout.
  double waitForOutput(int timeoutMs) {
    auto t0 = Clock::now();
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeoutMs);
    auto t1 = Clock::now();
    if (ret <= 0 || !(pfd.revents & POLLIN))
      return -1;
    // Drain the output so it doesn't pile up.
    char buf[4096];
    ::read(fd, buf, sizeof(buf));
    drain(5);
    return static_cast<double>(std::chrono::duration_cast<us>(t1 - t0).count());
  }
};

static std::string cxBinary() {
  if (std::filesystem::exists("./cx"))
    return "./cx";
  if (std::filesystem::exists("../build/cx"))
    return "../build/cx";
  return "./cx";
}

static void benchPty(std::size_t bufLines, std::size_t inserts) {
  auto bin = cxBinary();
  if (!std::filesystem::exists(bin)) {
    std::fprintf(stderr, "cx binary not found at %s, skipping PTY benchmark\n", bin.c_str());
    return;
  }

  auto content = generateContent(bufLines);
  TempFile f(content);
  Pty pty(30, 100, bin, {std::string(f.path())});

  // Wait for initial render.
  pty.drain(500);

  std::vector<double> durations;
  durations.reserve(inserts);

  for (std::size_t i = 0; i < inserts; ++i) {
    pty.send('x');
    double d = pty.waitForOutput(2000);
    if (d < 0) {
      std::fprintf(stderr, "PTY timeout at insert %zu\n", i);
      break;
    }
    durations.push_back(d);
  }

  if (durations.size() < 10) {
    std::fprintf(stderr, "Too few PTY samples\n");
    return;
  }

  std::vector<std::pair<double, std::size_t>> indexed;
  for (std::size_t i = 0; i < durations.size(); ++i)
    indexed.push_back({durations[i], i});

  auto stats = computeStats(durations);
  std::printf("PTY benchmark (%zu-line buffer, %zu inserts):\n", bufLines, durations.size());
  std::printf("  median: %6.0f us\n", stats.median);
  std::printf("  p95:    %6.0f us\n", stats.p95);
  std::printf("  p99:    %6.0f us\n", stats.p99);
  std::printf("  max:    %6.0f us\n", stats.max);

  std::sort(indexed.begin(), indexed.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::printf("  slowest 5:\n");
  for (int j = 0; j < 5 && j < static_cast<int>(indexed.size()); ++j)
    std::printf("    insert #%zu: %.0f us\n", indexed[j].second, indexed[j].first);

  // Quit the editor.
  ::write(pty.fd,
          "\x1b"
          "c"
          "n",
          3);
  pty.drain(500);
}

// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
  bool direct = false, pty = false;
  std::size_t lines = 500, inserts = 300;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--direct") == 0)
      direct = true;
    else if (std::strcmp(argv[i], "--pty") == 0)
      pty = true;
    else if (std::strcmp(argv[i], "--lines") == 0 && i + 1 < argc)
      lines = std::stoul(argv[++i]);
    else if (std::strcmp(argv[i], "--inserts") == 0 && i + 1 < argc)
      inserts = std::stoul(argv[++i]);
  }

  if (!direct && !pty) {
    direct = true;
    pty = true;
  }

  if (direct)
    benchDirect(lines, inserts);
  if (pty)
    benchPty(lines, inserts);
}
