#pragma once

/**
 * @file md/fence.hpp
 * @brief Fenced code block detection for Markdown buffers.
 */

#include "buffer/line_store.hpp"
#include "types.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

/**
 * @brief Which fenced code block a line-by-line scan is inside, if any.
 *
 * The one place that knows what opens and closes a fence, shared by
 * markdownFencedLines() and the Markdown tokenizer (which keeps one per
 * line in the syntax cache, hence the small fields).
 *
 * A fence opens with three or more backticks or tildes after optional
 * indentation (a backtick fence's info string can't contain a backtick)
 * and closes with a run of the same character, at least as long, on a
 * line of its own. An unclosed fence runs to the end of the buffer.
 * Fences inside block quotes are not recognised.
 */
struct MdFence {
  char ch = 0;           /**< '`' or '~' of the open fence; 0 outside one. */
  std::uint16_t len = 0; /**< Length of its opening run (capped at 65535). */

  /** @brief True while inside a fenced block. */
  bool open() const { return ch != 0; }

  /**
   * @brief Step over @p line, the line after the one last stepped over.
   * @return True if @p line belongs to a fenced block: an opening or
   *         closing fence line, or a line between them.
   */
  bool advance(std::string_view line);

  bool operator==(MdFence const&) const = default;
};

/**
 * @brief For every line, whether it belongs to a fenced code block (the
 *        opening and closing fence lines included); see MdFence.
 */
std::vector<bool> markdownFencedLines(LineStore const& lines);
