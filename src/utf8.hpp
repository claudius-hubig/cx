#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

/// Read the remaining bytes of a UTF-8 sequence from stdin, given the lead
/// byte. Appends continuation bytes to `dest`. Defined in utf8.cpp.
void readUtf8Tail(unsigned char lead, std::string& dest);

/// Read a complete UTF-8 codepoint from stdin given the lead byte.
/// For ASCII (< 0x80), returns the byte directly. For multi-byte sequences,
/// reads the continuation bytes from stdin and decodes. Defined in utf8.cpp.
uint32_t readUtf8Codepoint(unsigned char lead);
