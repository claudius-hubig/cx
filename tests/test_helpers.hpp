#pragma once

#include "buffer/buffer.hpp"
#include "buffer/buffer_sink.hpp"
#include "buffer/char_width.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// Trivial BufferSink stub for testing BufferManager without a terminal.
class NullBufferSink : public BufferSink {
public:
  void setBuffer(Buffer& buf) override { lastBuffer = &buf; }
  Buffer* lastBuffer = nullptr;
};

// RAII helper for a temporary file that is removed on destruction.
class TempFile {
public:
  explicit TempFile(std::string const& content = {}) {
    static std::atomic<int> counter{0};
    static auto seed = std::random_device{}();
    _path = std::filesystem::temp_directory_path() /
            ("cx_test_" + std::to_string(seed) + "_" + std::to_string(counter++));
    std::ofstream f(_path, std::ios::binary);
    f << content;
  }

  ~TempFile() { std::filesystem::remove(_path); }

  std::filesystem::path const& path() const { return _path; }

  TempFile(TempFile const&) = delete;
  TempFile& operator=(TempFile const&) = delete;

private:
  std::filesystem::path _path;
};

// Insert a string as UTF-8 codepoints into a buffer.
inline void insertString(Buffer& buf, std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    auto [cp, len] = decodeUtf8(s.data() + i, s.size() - i);
    buf.insert(cp);
    i += len;
  }
}

// Strip ANSI escape sequences from a string, returning only visible characters.
// Handles CSI sequences (ESC [ ... letter) and simple ESC-letter sequences.
inline std::string stripAnsi(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b') {
      ++i;
      if (i < s.size() && s[i] == '[') {
        // CSI sequence: skip until final byte (0x40-0x7E).
        ++i;
        while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E))
          ++i;
        if (i < s.size())
          ++i; // skip final byte
      } else if (i < s.size()) {
        ++i; // skip single char after ESC
      }
    } else {
      out += s[i];
      ++i;
    }
  }
  return out;
}

// Parse render output into a grid of rows, interpreting cursor-positioning
// escapes (\x1b[row;colH) to place characters at the correct positions.
// Returns a vector<string> with one entry per row (0-indexed), padded to
// the given number of columns with spaces.
inline std::vector<std::string> renderToGrid(std::string_view s, std::size_t rows,
                                             std::size_t cols) {
  std::vector<std::string> grid(rows, std::string(cols, ' '));
  std::size_t curRow = 0, curCol = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      // CSI sequence.
      i += 2;
      // Collect parameter bytes and intermediates.
      std::string params;
      while (i < s.size() && (s[i] < 0x40 || s[i] > 0x7E)) {
        params += s[i];
        ++i;
      }
      if (i >= s.size())
        break;
      char final_byte = s[i];
      ++i;

      if (final_byte == 'H') {
        // Cursor position: ESC[row;colH (1-based).
        std::size_t semi = params.find(';');
        if (semi != std::string::npos) {
          std::size_t r = std::stoul(params.substr(0, semi));
          std::size_t c = std::stoul(params.substr(semi + 1));
          if (r > 0)
            curRow = r - 1;
          if (c > 0)
            curCol = c - 1;
        }
      } else if (final_byte == 'K') {
        // Erase to end of line — fill rest of row with spaces.
        if (curRow < rows) {
          for (std::size_t c = curCol; c < cols; ++c)
            grid[curRow][c] = ' ';
        }
      } else if (final_byte == 'm') {
        // SGR — ignore (color/style).
      }
    } else if (s[i] == '\x1b') {
      // Non-CSI escape — skip ESC + next char.
      i += 2;
    } else {
      // Regular character — place on grid.
      unsigned char u = static_cast<unsigned char>(s[i]);
      if (curRow < rows && curCol < cols) {
        // Handle UTF-8 multi-byte sequences.
        if (u >= 0x80) {
          auto len = utf8SeqLen(u);
          if (i + len <= s.size()) {
            // Place the full UTF-8 sequence in the grid cell.
            grid[curRow][curCol] = s[i];
            // For multi-byte, we need to expand the cell. Instead, just
            // place bytes sequentially.
            std::string mb(s.data() + i, len);
            // Replace the single char with the full sequence.
            // Since grid cells are single chars, store the first byte and
            // advance curCol by the display width.
            auto [cp, cpLen] = decodeUtf8(s.data() + i, s.size() - i);
            std::size_t w = codepointWidth(cp);
            // Overwrite grid position(s).
            // We'll use a simpler approach: put the full string in the
            // first column position, mark subsequent positions.
            grid[curRow].replace(curCol, 1, mb);
            curCol += w;
            i += len;
            continue;
          }
        }
        grid[curRow][curCol] = s[i];
        ++curCol;
      }
      ++i;
    }
  }
  return grid;
}
