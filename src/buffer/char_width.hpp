#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

// Visual display width of a single ASCII byte (non-newline).
inline constexpr std::size_t charWidth(char ch) {
    auto u = static_cast<unsigned char>(ch);
    if (u == '\t')  return 2;  // rendered as two spaces
    if (u < 0x20 || u == 0x7f) return 2;  // ^X or \0
    return 1;
}

inline constexpr bool isUtf8Continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

// Number of bytes in a UTF-8 sequence starting with the given lead byte.
inline constexpr std::size_t utf8SeqLen(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1; // invalid or continuation — treat as single byte
}

// Decode a UTF-8 code point. Returns {codepoint, byte_length}.
inline constexpr std::pair<uint32_t, std::size_t> decodeUtf8(
        const char* data, std::size_t avail) {
    auto u = static_cast<unsigned char>(data[0]);
    if (u < 0x80) return {u, 1};
    if ((u & 0xE0) == 0xC0 && avail >= 2) {
        uint32_t cp = (u & 0x1F) << 6;
        cp |= (static_cast<unsigned char>(data[1]) & 0x3F);
        return {cp, 2};
    }
    if ((u & 0xF0) == 0xE0 && avail >= 3) {
        uint32_t cp = (u & 0x0F) << 12;
        cp |= (static_cast<unsigned char>(data[1]) & 0x3F) << 6;
        cp |= (static_cast<unsigned char>(data[2]) & 0x3F);
        return {cp, 3};
    }
    if ((u & 0xF8) == 0xF0 && avail >= 4) {
        uint32_t cp = (u & 0x07) << 18;
        cp |= (static_cast<unsigned char>(data[1]) & 0x3F) << 12;
        cp |= (static_cast<unsigned char>(data[2]) & 0x3F) << 6;
        cp |= (static_cast<unsigned char>(data[3]) & 0x3F);
        return {cp, 4};
    }
    return {u, 1}; // invalid/truncated, treat as raw byte
}

// Display width of a Unicode code point (1 or 2 cells).
inline constexpr std::size_t codepointWidth(uint32_t cp) {
    if (cp >= 0x1100 && cp <= 0x115F) return 2;  // Hangul Jamo
    if (cp >= 0x2E80 && cp <= 0x303E) return 2;  // CJK Radicals, Kangxi
    if (cp >= 0x3040 && cp <= 0x33BF) return 2;  // Hiragana, Katakana
    if (cp >= 0x3400 && cp <= 0x4DBF) return 2;  // CJK Extension A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;  // CJK Unified Ideographs
    if (cp >= 0xA000 && cp <= 0xA4CF) return 2;  // Yi
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 2;  // Hangul Syllables
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;  // CJK Compat. Ideographs
    if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;  // CJK Compat. Forms
    if (cp >= 0xFF01 && cp <= 0xFF60) return 2;  // Fullwidth Forms
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;  // Fullwidth Signs
    if (cp >= 0x20000 && cp <= 0x3134F) return 2; // CJK Extensions B-G
    return 1;
}

// Width and byte length of the character/code-point at data[0..avail).
struct CharInfo {
    std::size_t width; // display columns
    std::size_t len;   // bytes consumed
};

inline constexpr CharInfo charInfoAt(const char* data, std::size_t avail) {
    auto u = static_cast<unsigned char>(data[0]);
    if (u < 0x80) {
        if (u == '\t')  return {2, 1};
        if (u < 0x20 || u == 0x7f) return {2, 1};
        return {1, 1};
    }
    auto [cp, len] = decodeUtf8(data, avail);
    return {codepointWidth(cp), len};
}
