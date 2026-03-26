#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

/// Return true if b is a UTF-8 continuation byte: (b & 0xC0) == 0x80.
/// Pure predicate — works on any unsigned char value.
inline constexpr bool isUtf8Continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

/// Number of bytes in a UTF-8 sequence starting with the given lead byte.
/// Returns 1 for ASCII (<0x80), 2-4 for valid multi-byte lead bytes, and
/// 1 for invalid/continuation bytes (no error signaling).
inline constexpr std::size_t utf8SeqLen(unsigned char lead) {
  if (lead < 0x80)
    return 1;
  if ((lead & 0xE0) == 0xC0)
    return 2;
  if ((lead & 0xF0) == 0xE0)
    return 3;
  if ((lead & 0xF8) == 0xF0)
    return 4;
  return 1; // invalid or continuation — treat as single byte
}

/// Decode a UTF-8 code point starting at data[0].
/// Precondition: avail > 0 (at least one byte available).
/// Returns {codepoint, byte_length}. For malformed, truncated, overlong,
/// surrogate (U+D800–U+DFFF), or out-of-range (>U+10FFFF) sequences the
/// raw byte value is returned with length 1.
inline constexpr std::pair<uint32_t, std::size_t> decodeUtf8(char const* data, std::size_t avail) {
  auto u = static_cast<unsigned char>(data[0]);
  if (u < 0x80)
    return {u, 1};
  auto isCont = [](unsigned char b) { return (b & 0xC0) == 0x80; };
  if ((u & 0xE0) == 0xC0 && avail >= 2) {
    auto b1 = static_cast<unsigned char>(data[1]);
    if (!isCont(b1))
      return {u, 1};
    uint32_t cp = ((u & 0x1F) << 6) | (b1 & 0x3F);
    if (cp < 0x80)
      return {u, 1}; // overlong
    return {cp, 2};
  }
  if ((u & 0xF0) == 0xE0 && avail >= 3) {
    auto b1 = static_cast<unsigned char>(data[1]);
    auto b2 = static_cast<unsigned char>(data[2]);
    if (!isCont(b1) || !isCont(b2))
      return {u, 1};
    uint32_t cp = ((u & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
    if (cp < 0x800)
      return {u, 1}; // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF)
      return {u, 1}; // surrogate
    return {cp, 3};
  }
  if ((u & 0xF8) == 0xF0 && avail >= 4) {
    auto b1 = static_cast<unsigned char>(data[1]);
    auto b2 = static_cast<unsigned char>(data[2]);
    auto b3 = static_cast<unsigned char>(data[3]);
    if (!isCont(b1) || !isCont(b2) || !isCont(b3))
      return {u, 1};
    uint32_t cp = ((u & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
    if (cp < 0x10000)
      return {u, 1}; // overlong
    if (cp > 0x10FFFF)
      return {u, 1}; // out of range
    return {cp, 4};
  }
  return {u, 1}; // invalid/truncated, treat as raw byte
}

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
/// (tab/control -> width 2, printable -> 1). For ANSI CSI escape sequences
/// (ESC[ ... letter), returns width 0 and consumes the full sequence.
/// For valid multi-byte UTF-8, decodes and returns codepointWidth. Invalid
/// bytes (>= 0x80 with len == 1 from decodeUtf8) get width 2 for hex display.
inline constexpr CharInfo charInfoAt(char const* data, std::size_t avail) {
  auto u = static_cast<unsigned char>(data[0]);
  // ANSI CSI escape: ESC [ <params> <letter>
  if (u == 0x1b && avail >= 2 && data[1] == '[') {
    std::size_t i = 2;
    while (i < avail && !((static_cast<unsigned char>(data[i]) >= 0x40) &&
                          (static_cast<unsigned char>(data[i]) <= 0x7E)))
      ++i;
    if (i < avail)
      ++i; // consume the final letter
    return {0, i};
  }
  if (u < 0x80)
    return {charWidth(static_cast<char>(u)), 1};
  auto [cp, len] = decodeUtf8(data, avail);
  if (len == 1)
    return {2, 1}; // invalid byte — displayed as 2-digit hex
  return {codepointWidth(cp), len};
}

/// Visual (display column) width of a string, handling multi-byte UTF-8
/// and ANSI escape sequences (which contribute 0 width). Defined in utf8.cpp.
std::size_t visualWidth(std::string_view s);

/// Read the remaining bytes of a UTF-8 sequence from stdin, given the lead
/// byte. Appends continuation bytes to `dest`. Defined in utf8.cpp.
void readUtf8Tail(unsigned char lead, std::string& dest);

/// Read a complete UTF-8 codepoint from stdin given the lead byte.
/// For ASCII (< 0x80), returns the byte directly. For multi-byte sequences,
/// reads the continuation bytes from stdin and decodes. Defined in utf8.cpp.
uint32_t readUtf8Codepoint(unsigned char lead);
