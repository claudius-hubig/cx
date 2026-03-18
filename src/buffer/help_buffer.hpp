#pragma once

#include "buffer.hpp"

/// Readonly buffer displaying help text / keybindings. Populates _lines
/// with hardcoded content on construction. Overrides screenRow() and
/// render() to support side-by-side layout when the terminal is wide
/// enough.
class HelpBuffer : public Buffer {
public:
  /// Construct the help buffer and populate _lines with help content.
  HelpBuffer();

  bool readonly() const override { return true; }

  /// Screen row of the cursor, accounting for side-by-side layout
  /// when terminal width allows it.
  std::size_t screenRow(ColIdx cols, bool wrap = true) const override;

  /// Render help content with adaptive layout: stacked (single column)
  /// or side-by-side (commands + movement) depending on terminal width.
  std::tuple<RowIdx, ColIdx>
  render(std::string &out, RowIdx rows, ColIdx cols,
         bool showWhitespace = false, bool wrap = true) const override;
};
