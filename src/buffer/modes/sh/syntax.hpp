#pragma once

/**
 * @file sh/syntax.hpp
 * @brief Shell tokenizer (comments, strings, heredocs, $VAR expansions,
 *        keywords, operators).
 */

#include "buffer/modes/c/syntax.hpp"

#include <memory>
#include <string>
#include <vector>

/** @brief Shell tokenizer state carried between lines. */
struct ShSyntaxState {
  /**
   * @brief Shell heredoc tag: non-null means subsequent lines are heredoc
   *        body (String) until a line equal to the tag.
   *
   * Shared and immutable: a tag has no length limit, and every line of
   * the heredoc's body carries the same one, so copying the state from
   * line to line shares it instead of copying the string.
   */
  std::shared_ptr<std::string const> heredocTag;
  /** @brief Set for the `<<-` form, where leading tabs are stripped before the
   *         terminator comparison. */
  bool heredocStripTabs = false;
};

/**
 * @brief Tokenize a single shell (sh/bash/zsh/ksh) line, writing per-byte
 *        SyntaxToken values into @p out.
 *
 * @p out is resized/overwritten; reusing a vector avoids per-line heap
 * allocation.
 *
 * Multi-line state: @c state.heredocTag carries an open heredoc across
 * lines — while set, every subsequent line is one String span until a line
 * whose content equals the tag (leading tabs stripped first for the `<<-`
 * form, tracked by @c state.heredocStripTabs).
 */
void tokenizeShellLine(std::string const& line, ShSyntaxState& state,
                       std::vector<SyntaxToken>& out);
