#pragma once

#include "buffer.hpp"

#include <string>
#include <vector>

/// Readonly buffer that displays open buffers organized as a trie-based
/// directory tree. Collapses single-child subdirectories.
class BufferListBuffer : public Buffer {
public:
  /// Construct the buffer list from sorted buffer names. startName
  /// positions the cursor on the matching line (or line 0 if not found).
  BufferListBuffer(std::vector<std::string> names,
                   std::string const &startName = {});

  bool readonly() const override { return true; }

  /// Render the buffer list with reverse-video highlighting on the
  /// selected line.
  std::tuple<RowIdx, ColIdx>
  render(std::string &out, RowIdx rows, ColIdx cols,
         bool showWhitespace = false, bool wrap = true) const override;

  /// Return the full buffer name for the line the cursor is on.
  /// Returns empty string for directory header lines.
  std::string selectedName() const;

  /// Rebuild the display from a new set of buffer names. Safe to call
  /// repeatedly. Resets cursor to startName.
  void refresh(std::vector<std::string> names,
               std::string const &startName = {});

private:
  // Display text for each line (with indentation).
  std::vector<std::string> _displayLines;
  // Buffer name for each display line (empty for directory headers).
  std::vector<std::string> _lineToName;
};
