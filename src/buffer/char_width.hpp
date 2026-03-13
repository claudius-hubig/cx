#pragma once

#include <cstddef>

// Visual display width of a single (non-newline) character.
inline constexpr std::size_t charWidth(char ch) {
    auto u = static_cast<unsigned char>(ch);
    if (u == '\t')  return 2;  // rendered as two spaces
    if (u < 0x20 || u == 0x7f) return 2;  // ^X or \0
    return 1;
}
