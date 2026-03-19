#pragma once

#include "utf8.hpp"

#include <cstddef>
#include <cstdint>

/// True if c is a horizontal whitespace character (space or tab).
inline constexpr bool isWhitespace(char c) { return c == ' ' || c == '\t'; }

/// Visual display width of a single ASCII byte (excluding newline).
/// ASCII only: tab and control characters (including NUL and DEL) return 2
/// (rendered as two-character sequences like ^X or \0); printable bytes
/// (including space) return 1. Does not handle newline or multi-byte UTF-8
/// lead bytes — callers must check for those separately.
inline constexpr std::size_t charWidth(char ch) {
  auto u = static_cast<unsigned char>(ch);
  if (u == '\t')
    return 2; // rendered as two spaces
  if (u < 0x20 || u == 0x7f)
    return 2; // ^X or \0
  return 1;
}

/// Display width of a Unicode code point: 2 for CJK / wide ranges
/// (Hangul Jamo, CJK radicals, Hiragana/Katakana, CJK unified, Yi,
/// Hangul syllables, fullwidth forms, CJK extensions B-G), 1 otherwise.
/// Does not handle combining marks or zero-width characters.
inline constexpr std::size_t codepointWidth(uint32_t cp) {
  if (cp >= 0x1100 && cp <= 0x115F)
    return 2; // Hangul Jamo
  if (cp >= 0x2E80 && cp <= 0x303E)
    return 2; // CJK Radicals, Kangxi
  if (cp >= 0x3040 && cp <= 0x33BF)
    return 2; // Hiragana, Katakana
  if (cp >= 0x3400 && cp <= 0x4DBF)
    return 2; // CJK Extension A
  if (cp >= 0x4E00 && cp <= 0x9FFF)
    return 2; // CJK Unified Ideographs
  if (cp >= 0xA000 && cp <= 0xA4CF)
    return 2; // Yi
  if (cp >= 0xAC00 && cp <= 0xD7AF)
    return 2; // Hangul Syllables
  if (cp >= 0xF900 && cp <= 0xFAFF)
    return 2; // CJK Compat. Ideographs
  if (cp >= 0xFE30 && cp <= 0xFE6F)
    return 2; // CJK Compat. Forms
  if (cp >= 0xFF01 && cp <= 0xFF60)
    return 2; // Fullwidth Forms
  if (cp >= 0xFFE0 && cp <= 0xFFE6)
    return 2; // Fullwidth Signs
  if (cp >= 0x20000 && cp <= 0x3134F)
    return 2; // CJK Extensions B-G
  return 1;
}

/// Width and byte length of the character/code-point at data[0..avail).
struct CharInfo {
  std::size_t width; // display columns
  std::size_t len;   // bytes consumed
};

/// Combined decode + width for the character at data[0..avail).
/// Precondition: avail > 0. For ASCII bytes, applies charWidth rules
/// (tab/control → width 2, printable → 1). For valid multi-byte UTF-8,
/// decodes and returns codepointWidth. Invalid bytes (>= 0x80 with
/// len == 1 from decodeUtf8) get width 2 for hex display.
inline constexpr CharInfo charInfoAt(char const* data, std::size_t avail) {
  auto u = static_cast<unsigned char>(data[0]);
  if (u < 0x80)
    return {charWidth(static_cast<char>(u)), 1};
  auto [cp, len] = decodeUtf8(data, avail);
  if (len == 1)
    return {2, 1}; // invalid byte — displayed as 2-digit hex
  return {codepointWidth(cp), len};
}
