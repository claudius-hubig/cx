#pragma once

#include "buffer/buffer.hpp"
#include "buffer/buffer_sink.hpp"
#include "buffer/buffer_view.hpp"
#include "types.hpp"
#include "utf8.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Convert a `vector<string>` to a `vector<LinePtr>` for tests that
// construct ad-hoc input for the bracket matchers (which now take
// `vector<LinePtr> const&`).
inline std::vector<LinePtr> toLinePtrs(std::vector<std::string> const& v) {
  std::vector<LinePtr> out;
  out.reserve(v.size());
  for (auto const& s : v)
    out.push_back(std::make_shared<std::string const>(s));
  return out;
}

// Test wrapper that bundles any Buffer subclass with an owned BufferView and
// provides forwarding methods for all view-state operations. This allows
// tests written against the old Buffer API (buf.insert, buf.moveLeft, ...)
// to keep working after view state was split out into BufferView.
//
// Usage: `TB<TextBuffer> buf("name");` then call view-style methods on buf.
template <typename T> class TB : public T {
public:
  using T::T;

  // The embedded view. Public so tests can pass it to free functions.
  mutable BufferView view{*static_cast<Buffer*>(this)};

  // --- View state accessors ---
  BufferPoint point() const { return view.point(); }
  std::optional<BufferPoint> mark() const { return view.mark(); }
  BufferPoint viewFrom() const { return view.viewFrom(); }
  ViewFrom const& viewFromRaw() const { return view.viewFromRaw(); }
  std::optional<Highlight> const& highlight() const { return view.highlight(); }
  std::tuple<std::size_t, std::size_t> lineCol() const { return view.lineCol(); }

  void setPoint(std::size_t line, std::size_t offset) { view.setPoint(line, offset); }
  void setMark(BufferPoint p) { view.setMark(p); }
  void clearMark() { view.clearMark(); }
  void toggleMark() { view.toggleMark(); }
  void setHighlight(BufferPoint from, BufferPoint to) { view.setHighlight(from, to); }
  void clearHighlight() { view.clearHighlight(); }

  // --- Movement ---
  void moveLeft() { view.moveLeft(); }
  void moveRight() { view.moveRight(); }
  void moveUp(ColIdx cols) { view.moveUp(cols); }
  void moveDown(ColIdx cols) { view.moveDown(cols); }
  void movePhysicalUp() { view.movePhysicalUp(); }
  void movePhysicalDown() { view.movePhysicalDown(); }
  void moveHome() { view.moveHome(); }
  void moveEnd() { view.moveEnd(); }
  void moveWordRight() { view.moveWordRight(); }
  void moveWordLeft() { view.moveWordLeft(); }
  void moveParagraphUp() { view.moveParagraphUp(); }
  void moveParagraphDown() { view.moveParagraphDown(); }
  void pageUp(RowIdx rows, ColIdx cols, bool wrap) { view.pageUp(rows, cols, wrap); }
  void pageDown(RowIdx rows, ColIdx cols, bool wrap) { view.pageDown(rows, cols, wrap); }
  bool gotoMatchingBracket() { return view.gotoMatchingBracket(); }
  void gotoLine(std::size_t line, RowIdx rows) { view.gotoLine(line, rows); }
  void scrollDown() { view.scrollDown(); }
  void scrollUp(bool wrap = false) { view.scrollUp(wrap); }
  void scrollViewNearEnd(RowIdx rows) { view.scrollViewNearEnd(rows); }

  // --- Editing ---
  void insert(uint32_t cp) { view.insert(cp); }
  void deleteAtPoint() { view.deleteAtPoint(); }
  void deleteBeforePoint() { view.deleteBeforePoint(); }
  void deleteChunkAtPoint() { view.deleteChunkAtPoint(); }
  void deleteChunkBeforePoint() { view.deleteChunkBeforePoint(); }
  void adjustCursorToIndent() { view.adjustCursorToIndent(); }

  // --- Rendering ---
  void ensureVisible(RowIdx rows, ColIdx cols, bool wrap) { view.ensureVisible(rows, cols, wrap); }
  bool currentLineWraps(ColIdx cols) const { return view.currentLineWraps(cols); }
  // Note: screenRow is NOT forwarded so that Buffer's virtual
  // screenRow(cols, point, viewFrom, wrap) stays accessible.
};

// Trivial BufferSink stub for testing BufferManager without a terminal.
class NullBufferSink : public BufferSink {
public:
  void setBuffer(Buffer& buf) override { lastBuffer = &buf; }
  Buffer* lastBuffer = nullptr;
};

// RAII helper for a temporary file that is removed on destruction.
class TempFile {
public:
  explicit TempFile(std::string const& content = {}, std::string const& suffix = {}) {
    static std::atomic<int> counter{0};
    static auto seed = std::random_device{}();
    _path = std::filesystem::temp_directory_path() /
            ("cx_test_" + std::to_string(seed) + "_" + std::to_string(counter++) + suffix);
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

// RAII helper for a temporary directory (with optional files) removed on destruction.
class TempDir {
public:
  explicit TempDir(std::vector<std::string> const& filenames = {}) {
    static std::atomic<int> counter{0};
    static auto seed = std::random_device{}();
    _path = std::filesystem::temp_directory_path() /
            ("cx_testdir_" + std::to_string(seed) + "_" + std::to_string(counter++));
    std::filesystem::create_directories(_path);
    for (auto& name : filenames) {
      std::ofstream f(_path / name);
      f << name; // write the filename as content
    }
  }

  ~TempDir() { std::filesystem::remove_all(_path); }

  std::filesystem::path const& path() const { return _path; }

  TempDir(TempDir const&) = delete;
  TempDir& operator=(TempDir const&) = delete;

private:
  std::filesystem::path _path;
};

// Create RenderParams from a BufferView's current state.
inline RenderParams renderParams(BufferView const& view, bool showWhitespace = false,
                                 bool wrap = true, std::size_t rowOffset = 0,
                                 std::size_t colOffset = 0) {
  RenderParams p;
  p.point = view.point();
  p.viewFrom = view.viewFromRaw();
  p.mark = view.mark();
  p.highlight = view.highlight();
  p.extraCursors = view.extraCursors();
  p.showWhitespace = showWhitespace;
  p.wrap = wrap;
  p.rowOffset = rowOffset;
  p.colOffset = colOffset;
  return p;
}

// Overload for TB wrapper: reads from its embedded view.
template <typename T>
inline RenderParams renderParams(TB<T> const& buf, bool showWhitespace = false, bool wrap = true,
                                 std::size_t rowOffset = 0, std::size_t colOffset = 0) {
  return renderParams(buf.view, showWhitespace, wrap, rowOffset, colOffset);
}

// Insert a string as UTF-8 codepoints through a BufferView.
inline void insertString(BufferView& view, std::string_view s) {
  for (std::size_t i = 0; i < s.size();) {
    auto [cp, len] = decodeUtf8(s.data() + i, s.size() - i);
    view.insert(cp);
    i += len;
  }
}

// Overload for TB wrapper: forwards to its embedded view.
template <typename T> inline void insertString(TB<T>& buf, std::string_view s) {
  insertString(buf.view, s);
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
            if (isCombiningMark(cp) || isEscapedCodepoint(cp)) {
              // Zero width, like xfce4-terminal: joins the previous cell.
              grid[curRow].insert(curCol, mb);
              i += len;
              continue;
            }
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
