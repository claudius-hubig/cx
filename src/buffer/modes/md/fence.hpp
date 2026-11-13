#pragma once

/**
 * @file md/fence.hpp
 * @brief Fenced code block detection for Markdown buffers.
 */

#include "types.hpp"

#include <vector>

/**
 * @brief For every line, whether it belongs to a fenced code block (the
 *        opening and closing fence lines included).
 *
 * A fence opens with three or more backticks or tildes after optional
 * indentation (a backtick fence's info string can't contain a backtick)
 * and closes with a run of the same character, at least as long, on a
 * line of its own. An unclosed fence runs to the end of the buffer.
 * Fences inside block quotes are not recognised.
 */
std::vector<bool> markdownFencedLines(std::vector<LinePtr> const& lines);
