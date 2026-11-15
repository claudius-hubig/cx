#pragma once

/**
 * @file syntax_state.hpp
 * @brief SyntaxState — the cross-line tokenizer state of any mode, as kept
 *        per line by the buffer's syntax cache.
 */

#include "modes/c/syntax.hpp"
#include "modes/md/syntax.hpp"
#include "modes/py/syntax.hpp"
#include "modes/sh/syntax.hpp"
#include "modes/tml/syntax.hpp"
#include "modes/yml/syntax.hpp"

#include <variant>

/**
 * @brief The state one mode's tokenizer carries from a line to the next.
 *
 * A variant so that a cache entry is only as large as the largest mode's
 * state. Modes without cross-line state (txt, dif, jsn) hold
 * std::monostate. Every line of a buffer holds the alternative of the
 * buffer's mode; use syntaxStateFor() to reach it.
 */
using SyntaxState = std::variant<std::monostate, CSyntaxState, MdSyntaxState, ShSyntaxState,
                                 PySyntaxState, YmlSyntaxState, TmlSyntaxState>;

/**
 * @brief @p state as mode state @p S, reset to a default @p S first if it
 *        holds another alternative (the start of a buffer, or a mode change).
 */
template <class S> S& syntaxStateFor(SyntaxState& state) {
  if (auto* s = std::get_if<S>(&state))
    return *s;
  return state.emplace<S>();
}
