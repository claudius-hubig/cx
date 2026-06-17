#pragma once

/**
 * @file help_buffer.hpp
 * @brief HelpBuffer — a read-only buffer rendering the keybinding
 *        reference, generated from the shortcut binding table.
 */

#include "buffer.hpp"

/**
 * @brief Read-only buffer displaying help text / keybindings.
 *
 * Populates its lines with content on construction and overrides
 * screenRow() and render() to lay out command and movement sections
 * side-by-side when the terminal is wide enough, or stacked otherwise.
 */
class HelpBuffer : public Buffer {
public:
  /** @brief Construct the help buffer and populate its content. */
  HelpBuffer();

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }

  /**
   * @brief Screen row of the cursor, accounting for side-by-side layout
   *        when terminal width allows it.
   */
  std::size_t screenRow(ColIdx cols, BufferPoint point, ViewFrom viewFrom,
                        bool wrap = true) const override;

  /**
   * @brief Render help content with adaptive layout.
   *
   * Single-column (stacked) or side-by-side (commands + movement)
   * depending on terminal width.
   */
  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    RenderParams const& params) const override;
};
