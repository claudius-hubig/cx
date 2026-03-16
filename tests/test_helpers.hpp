#pragma once

#include "buffer/buffer_sink.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

// Trivial BufferSink stub for testing BufferManager without a terminal.
class NullBufferSink : public BufferSink {
public:
  void setBuffer(Buffer &buf) override { lastBuffer = &buf; }
  Buffer *lastBuffer = nullptr;
};

// RAII helper for a temporary file that is removed on destruction.
class TempFile {
public:
  explicit TempFile(std::string const &content = {}) {
    static std::atomic<int> counter{0};
    static auto seed = std::random_device{}();
    _path =
        std::filesystem::temp_directory_path() /
        ("cx_test_" + std::to_string(seed) + "_" + std::to_string(counter++));
    std::ofstream f(_path, std::ios::binary);
    f << content;
  }

  ~TempFile() { std::filesystem::remove(_path); }

  std::filesystem::path const &path() const { return _path; }

  TempFile(TempFile const &) = delete;
  TempFile &operator=(TempFile const &) = delete;

private:
  std::filesystem::path _path;
};
