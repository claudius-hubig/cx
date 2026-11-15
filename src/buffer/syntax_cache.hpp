#pragma once

/**
 * @file syntax_cache.hpp
 * @brief SyntaxCache — a buffer's per-line tokens and cross-line tokenizer
 *        state, stored for the buffer's mode only.
 */

#include "modes/c/syntax.hpp"
#include "modes/md/syntax.hpp"
#include "modes/py/syntax.hpp"
#include "modes/sh/syntax.hpp"
#include "modes/tml/syntax.hpp"
#include "modes/yml/syntax.hpp"

#include <variant>
#include <vector>

/**
 * @brief One line's syntax cache entry for a mode whose cross-line
 *        tokenizer state is @p S.
 */
template <class S> struct LineSyntax {
  std::vector<SyntaxToken> tokens; /**< Per-byte token classification. */
  /** @brief State at the end of the line, which feeds the next line (no
   *         space for std::monostate). */
  [[no_unique_address]] S stateAfter;
};

/**
 * @brief A buffer's syntax cache: one entry per line, all of the buffer
 *        mode's state type.
 *
 * The variant is chosen per buffer, not per line, so entries carry no tag
 * and each is only as large as its own mode needs. Modes without
 * cross-line state (txt, dif, jsn) use std::monostate.
 */
using SyntaxCache =
    std::variant<std::vector<LineSyntax<std::monostate>>, std::vector<LineSyntax<CSyntaxState>>,
                 std::vector<LineSyntax<MdSyntaxState>>, std::vector<LineSyntax<ShSyntaxState>>,
                 std::vector<LineSyntax<PySyntaxState>>, std::vector<LineSyntax<YmlSyntaxState>>,
                 std::vector<LineSyntax<TmlSyntaxState>>>;
