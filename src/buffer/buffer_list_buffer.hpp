#pragma once

/**
 * @file buffer_list_buffer.hpp
 * @brief BufferListBuffer — a read-only buffer listing the open buffers as
 *        a collapsed trie-based directory tree.
 */

#include "buffer.hpp"

#include <string>
#include <vector>

/**
 * @brief Read-only buffer that displays open buffers as a trie-based
 *        directory tree, collapsing single-child subdirectories.
 */
class BufferListBuffer : public Buffer {
public:
  /**
   * @brief Build the listing from sorted buffer and directory names.
   *
   * Directory buffers are merged into the trie as selectable directory
   * headers rather than separate leaf entries.
   * @param names Open text-buffer names (sorted).
   * @param dirNames Open directory-buffer names (sorted).
   * @param startName Positions the cursor on the matching line (or line 0).
   */
  BufferListBuffer(std::vector<std::string> names, std::vector<std::string> dirNames = {},
                   std::string const& startName = {});

  bool readonly() const override { return true; }
  bool supportsLineNumberGutter() const override { return false; }
  bool selectsRows() const override { return true; }

  /** @brief Render the list with reverse-video on the selected line. */
  std::tuple<RowIdx, ColIdx> render(std::string& out, RowIdx rows, ColIdx cols,
                                    RenderParams const& params) const override;

  /**
   * @brief Full buffer name for the line at @p p.
   * @return The buffer name, or an empty string for directory header lines.
   */
  std::string selectedName(BufferPoint p) const;

  /**
   * @brief Rebuild the display from a new set of names.
   *
   * Safe to call repeatedly.
   * @return A ViewHint positioning the cursor on @p startName.
   */
  ViewHint refresh(std::vector<std::string> names, std::vector<std::string> dirNames = {},
                   std::string const& startName = {});

private:
  std::vector<std::string> _displayLines; /**< Display text per line (with indentation). */
  std::vector<std::string> _lineToName;   /**< Buffer name per line (empty for dir headers). */
};
