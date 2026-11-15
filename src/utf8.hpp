#pragma once

/**
 * @file utf8.hpp
 * @brief UTF-8 decoding/encoding, character classification, display-width
 *        measurement, and visual-row layout helpers.
 *
 * The constexpr predicates and decode/encode routines are the single
 * place that knows the UTF-8 bit patterns. The display-width and
 * visual-row functions (defined in utf8.cpp) measure text exactly as
 * Buffer::render draws it, so cursor, scroll, and wrap math stay in sync.
 */

#include "unicode_tables.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief True if @p b is a UTF-8 continuation byte ((b & 0xC0) == 0x80).
 *
 * Pure predicate that works on any unsigned char value. This is the
 * single place that knows the continuation-byte bit pattern.
 */
inline constexpr bool isUtf8Continuation(unsigned char b) { return (b & 0xC0) == 0x80; }

/**
 * @brief The low six payload bits a UTF-8 continuation byte carries.
 *
 * Also the per-byte group emitted when encoding.
 */
inline constexpr unsigned char kUtf8ContValueMask = 0x3F;

/**
 * @brief Number of bytes in the UTF-8 sequence starting with @p lead.
 *
 * Returns 1 for ASCII (<0x80), 2-4 for valid multi-byte lead bytes, and
 * 1 for invalid/continuation bytes (no error signalling). This is the
 * single place that classifies a lead byte by its high bits.
 */
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

/**
 * @brief Smallest code point that legitimately needs an N-byte sequence,
 *        indexed by sequence length.
 *
 * Entries 0/1 are unused (ASCII has no minimum). Shared by the decoder
 * (overlong rejection) and the encoder (length selection) so these
 * boundaries live in exactly one place.
 */
inline constexpr std::array<uint32_t, 5> kUtf8MinForLen{0, 0, 0x80, 0x800, 0x10000};

/** @brief Lowest UTF-16 surrogate code point; not well-formed in UTF-8. */
inline constexpr uint32_t kSurrogateMin = 0xD800;
/** @brief Highest UTF-16 surrogate code point; not well-formed in UTF-8. */
inline constexpr uint32_t kSurrogateMax = 0xDFFF;
/** @brief Largest Unicode scalar value. */
inline constexpr uint32_t kMaxCodepoint = 0x10FFFF;

/** @brief True if @p cp falls in the UTF-16 surrogate range. */
inline constexpr bool isSurrogate(uint32_t cp) {
  return cp >= kSurrogateMin && cp <= kSurrogateMax;
}

/**
 * @brief Decode the UTF-8 code point starting at @p data[0].
 *
 * For malformed, truncated, overlong, surrogate (U+D800–U+DFFF), or
 * out-of-range (>U+10FFFF) sequences the raw byte value is returned with
 * length 1.
 *
 * @param data Pointer to the first byte of the sequence.
 * @param avail Bytes available at @p data.
 * @pre @p avail > 0.
 * @return {codepoint, byte_length}.
 */
inline constexpr std::pair<uint32_t, std::size_t> decodeUtf8(char const* data, std::size_t avail) {
  auto u = static_cast<unsigned char>(data[0]);
  std::size_t len = utf8SeqLen(u);
  if (len == 1)
    return {u, 1}; // ASCII, or an invalid/continuation lead — raw byte
  if (avail < len)
    return {u, 1}; // truncated
  // The lead byte's payload occupies its low (7 - len) bits, i.e. the
  // 0x1F/0x0F/0x07 masks for len 2/3/4 — exactly 0x7F >> len.
  uint32_t cp = u & static_cast<unsigned char>(0x7F >> len);
  for (std::size_t k = 1; k < len; ++k) {
    auto b = static_cast<unsigned char>(data[k]);
    if (!isUtf8Continuation(b))
      return {u, 1};
    cp = (cp << 6) | (b & kUtf8ContValueMask);
  }
  // Reject overlong, surrogate, and out-of-range encodings. The
  // surrogate/range tests only bite for the lengths that can actually
  // reach those code points, so one shared check covers every length.
  if (cp < kUtf8MinForLen[len] || isSurrogate(cp) || cp > kMaxCodepoint)
    return {u, 1};
  return {cp, len};
}

/**
 * @brief Largest code-point boundary <= @p off in @p s.
 *
 * Steps back over at most three continuation bytes (the longest valid
 * tail), so a run of stray continuation bytes in invalid UTF-8 costs O(1).
 * A continuation byte that the sequence before it does not reach (it
 * follows a complete, truncated or invalid sequence) is a character of
 * its own, as decodeUtf8() reads it, and is its own boundary.
 * @pre @p off <= s.size().
 */
inline constexpr std::size_t snapToCodepoint(std::string_view s, std::size_t off) {
  if (off >= s.size())
    return off;
  std::size_t lead = off;
  for (int k = 0; k < 3 && lead > 0 && isUtf8Continuation(static_cast<unsigned char>(s[lead])); ++k)
    --lead;
  return lead + decodeUtf8(s.data() + lead, s.size() - lead).second > off ? lead : off;
}

/**
 * @brief Encode a Unicode code point as UTF-8.
 *
 * Surrogate code points (U+D800..U+DFFF) and code points above U+10FFFF
 * are rejected and encoded as the replacement character (U+FFFD).
 *
 * @param cp The code point to encode.
 * @return {byte_count, bytes}. Only the first @c byte_count array entries
 *         are valid.
 */
inline constexpr std::pair<std::size_t, std::array<unsigned char, 4>> encodeUtf8(uint32_t cp) {
  // Surrogates and out-of-range values are not valid scalar values:
  // normalise to U+FFFD up front so the single encode path below emits
  // the replacement character, rather than spelling its bytes twice.
  if (isSurrogate(cp) || cp > kMaxCodepoint)
    cp = 0xFFFD;
  std::array<unsigned char, 4> buf{};
  if (cp < kUtf8MinForLen[2]) {
    buf[0] = static_cast<unsigned char>(cp);
    return {1, buf};
  }
  std::size_t len = cp < kUtf8MinForLen[3] ? 2 : cp < kUtf8MinForLen[4] ? 3 : 4;
  // Lead byte high bits: 0xC0/0xE0/0xF0 for len 2/3/4. Each continuation
  // byte then carries one six-bit group, most significant first.
  static constexpr unsigned char kLeadPrefix[5] = {0, 0, 0xC0, 0xE0, 0xF0};
  buf[0] = static_cast<unsigned char>(kLeadPrefix[len] | (cp >> (6 * (len - 1))));
  for (std::size_t k = 1; k < len; ++k)
    buf[k] = static_cast<unsigned char>(0x80 | ((cp >> (6 * (len - 1 - k))) & kUtf8ContValueMask));
  return {len, buf};
}

/** @brief True if @p c is horizontal whitespace (space or tab). */
inline constexpr bool isWhitespace(char c) { return c == ' ' || c == '\t'; }

/**
 * @brief Visual display width of a single ASCII byte (excluding newline).
 *
 * ASCII only: tab and control characters (including NUL and DEL) return 2
 * (rendered as two-character sequences like ^X or \\0); printable bytes
 * (including space) return 1. Does not handle newline or multi-byte
 * UTF-8 lead bytes — callers must check for those separately.
 */
inline constexpr std::size_t charWidth(char ch) {
  auto u = static_cast<unsigned char>(ch);
  if (u == '\t')
    return 2; // rendered as two spaces
  if (u < 0x20 || u == 0x7f)
    return 2; // ^X or \0
  return 1;
}

/** @brief kBlockClass value for a 64-code-point block of mixed classes. */
inline constexpr uint8_t kMixedBlock = 0xFF;

/**
 * @brief Per block of 64 code points (index cp >> 6): the CodepointClass
 *        all of them share, or kMixedBlock. Derived from kCodepointClasses
 *        at compile time, so the two can't disagree; it lets the common
 *        alphabets and ideographs skip the binary search.
 */
inline constexpr auto kBlockClass = [] {
  std::array<uint8_t, (kMaxCodepoint >> 6) + 1> blocks{}; // 0 == Normal
  for (auto const& r : kCodepointClasses)
    for (uint32_t b = r.first >> 6; b <= r.last >> 6; ++b) {
      bool whole = r.first <= (b << 6) && r.last >= (b << 6) + 63;
      blocks[b] = whole ? static_cast<uint8_t>(r.cls) : kMixedBlock;
    }
  return blocks;
}();
static_assert(static_cast<uint8_t>(CodepointClass::Normal) == 0);

/**
 * @brief Display class of a code point (Unicode 15.1, as xfce4-terminal
 *        measures it). Code points above kMaxCodepoint are Normal.
 */
inline constexpr CodepointClass codepointClass(uint32_t cp) {
  if (cp > kMaxCodepoint)
    return CodepointClass::Normal;
  if (auto block = kBlockClass[cp >> 6]; block != kMixedBlock)
    return static_cast<CodepointClass>(block);
  std::size_t lo = 0, hi = kCodepointClasses.size();
  while (lo < hi) {
    auto mid = (lo + hi) / 2;
    if (kCodepointClasses[mid].first <= cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo > 0 && cp <= kCodepointClasses[lo - 1].last ? kCodepointClasses[lo - 1].cls
                                                        : CodepointClass::Normal;
}

/**
 * @brief Display width of a code point on its own: 2 for
 *        CodepointClass::Wide, else 1.
 *
 * Zero-width marks and escaped characters are handled by charInfoAt().
 */
inline constexpr std::size_t codepointWidth(uint32_t cp) {
  return codepointClass(cp) == CodepointClass::Wide ? 2 : 1;
}

/** @brief Width and byte length of the character at @c data[0..avail). */
struct CharInfo {
  std::size_t width; /**< Display columns. */
  std::size_t len;   /**< Bytes consumed. */
};

/**
 * @brief True for a combining mark: zero width, drawn onto the preceding
 *        character (Mn, Me, Hangul medial/final jamo, variation selectors).
 */
inline constexpr bool isCombiningMark(uint32_t cp) {
  return codepointClass(cp) == CodepointClass::Combining;
}

/**
 * @brief True for a character cx shows as an escape instead of sending it
 *        to the terminal: invisible format characters (zero-width space,
 *        ZWJ/ZWNJ, BOM, direction marks and bidi controls, soft hyphen,
 *        ...), the line and paragraph separators, and the C1 controls
 *        (which a terminal may act on).
 */
inline constexpr bool isEscapedCodepoint(uint32_t cp) {
  return codepointClass(cp) == CodepointClass::Escaped;
}

/**
 * @brief Width of the escape codepointEscape() writes for @p cp:
 *        "\\u200b" (6) for the BMP, "\\u{e0001}" (9 or 10) above it.
 */
inline constexpr std::size_t codepointEscapeWidth(uint32_t cp) {
  if (cp <= 0xFFFF)
    return 6;
  return cp <= 0xFFFFF ? 9 : 10;
}

/** @brief The escape text for @p cp, e.g. "\\u200b". */
std::string codepointEscape(uint32_t cp);

/** @brief Display text of a combining mark with no character to sit on. */
inline constexpr std::string_view kOrphanMarkBase = "\xe2\x97\x8c"; // ◌ U+25CC

/**
 * @brief Width and byte length of the cluster at @c data[0..avail): one
 *        character plus the combining marks that follow it.
 *
 * Measures exactly what Buffer::render's emitChar draws, and is the one
 * place that decides where a cluster ends, so rendering, wrapping,
 * cursor columns and movement never split a character from its marks.
 *
 * - Printable ASCII and valid multi-byte characters take codepointWidth()
 *   and absorb the combining marks after them (which add no width, as in
 *   xfce4-terminal).
 * - Tab and control bytes (ESC included) are width 2, drawn "^X"; an
 *   invalid byte (len == 1 from decodeUtf8) is width 4, drawn "\\xHH"; a
 *   escaped character is drawn as codepointEscape(). None of these
 *   absorb marks.
 * - Combining marks with nothing to attach to (line start, or after one
 *   of the above) form their own cluster, drawn on kOrphanMarkBase:
 *   width 1.
 *
 * A raw ESC is treated as an ordinary content control byte (the renderer
 * never passes a content ESC through to the terminal). To measure a
 * string that already contains renderer-emitted ANSI escapes (the
 * modeline, escapeNonPrintable output), use displayCharInfoAt(), which
 * skips CSI sequences as zero-width.
 *
 * @pre @p avail > 0.
 */
inline constexpr CharInfo charInfoAt(char const* data, std::size_t avail) {
  CharInfo info{1, 1};
  auto u = static_cast<unsigned char>(data[0]);
  if (u < 0x80) {
    info.width = charWidth(static_cast<char>(u));
    if (info.width != 1)
      return info; // tab or control byte
  } else {
    auto [cp, len] = decodeUtf8(data, avail);
    if (len == 1)
      return {4, 1}; // invalid byte — displayed as "\xHH"
    switch (codepointClass(cp)) {
    case CodepointClass::Escaped:
      return {codepointEscapeWidth(cp), len};
    case CodepointClass::Wide:
      info = {2, len};
      break;
    case CodepointClass::Normal:
    case CodepointClass::Combining: // nothing to sit on: drawn on kOrphanMarkBase
      info = {1, len};
      break;
    }
  }
  // Absorb trailing marks. Nothing below U+0300 (lead byte 0xCC) is one.
  while (info.len < avail && static_cast<unsigned char>(data[info.len]) >= 0xCC) {
    auto [cp, len] = decodeUtf8(data + info.len, avail - info.len);
    if (len == 1 || !isCombiningMark(cp))
      break;
    info.len += len;
  }
  return info;
}

/**
 * @brief Start of the cluster (see charInfoAt()) containing byte @p off.
 *
 * Steps back over combining marks to their base, then forward cluster by
 * cluster, so the answer always agrees with charInfoAt().
 * @pre @p off <= s.size(); returns s.size() for @p off == s.size().
 */
inline constexpr std::size_t clusterStart(std::string_view s, std::size_t off) {
  off = snapToCodepoint(s, off);
  if (off >= s.size())
    return off;
  std::size_t base = off;
  while (base > 0 && isCombiningMark(decodeUtf8(s.data() + base, s.size() - base).first))
    base = snapToCodepoint(s, base - 1);
  for (;;) {
    auto len = charInfoAt(s.data() + base, s.size() - base).len;
    if (base + len > off)
      return base;
    base += len;
  }
}

/**
 * @brief Start of the cluster just before byte @p off.
 * @pre 0 < @p off <= s.size().
 */
inline constexpr std::size_t prevClusterStart(std::string_view s, std::size_t off) {
  return clusterStart(s, off - 1);
}

/**
 * @brief Start of the cluster after the one at byte @p off.
 * @pre @p off < s.size() and is a cluster start.
 */
inline constexpr std::size_t nextClusterStart(std::string_view s, std::size_t off) {
  return off + charInfoAt(s.data() + off, s.size() - off).len;
}

/**
 * @brief Like charInfoAt(), but recognises an editor-emitted ANSI CSI
 *        escape (ESC '[' … final-byte 0x40..0x7E) as a single zero-width,
 *        full-length pass-through.
 *
 * Use ONLY on already-rendered strings (the modeline status line,
 * escapeNonPrintable output), never on raw buffer content: emitChar
 * escapes a content ESC to a visible "^[", so swallowing an "ESC [" run
 * there would drop trailing bytes and desync the width / cursor / wrap
 * math.
 */
inline constexpr CharInfo displayCharInfoAt(char const* data, std::size_t avail) {
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
  return charInfoAt(data, avail);
}

/**
 * @brief Visual (display-column) width of a string, accounting for
 *        multi-byte UTF-8 and ANSI escape sequences (zero width).
 *
 * Defined in utf8.cpp.
 */
std::size_t visualWidth(std::string_view s);

/**
 * @brief Return @p s with any non-printable byte replaced by a visible
 *        "\\xHH" escape so it cannot be silently swallowed by the terminal.
 *
 * Bytes rendered as escapes are: ASCII control characters (< 0x20,
 * including TAB and newline), DEL (0x7F), and any high byte (>= 0x80) that
 * does not start a valid UTF-8 sequence. Escaped characters (see
 * isEscapedCodepoint()) become codepointEscape() escapes and a combining mark with nothing to
 * sit on gets kOrphanMarkBase, matching charInfoAt(). Other printable
 * ASCII and valid multi-byte UTF-8 pass through unchanged. Intended for filenames and
 * other short user-visible strings from outside the editor; not for
 * buffer text content, which the buffer renderer styles separately.
 * Defined in utf8.cpp.
 */
std::string escapeNonPrintable(std::string_view s);

/**
 * @brief Truncate @p s from the front so its visual width is at most
 *        @p maxWidth, prepending a horizontal ellipsis ("…", visual width
 *        1) when content was dropped.
 *
 * Returns @p s unchanged if it already fits, or an empty string when
 * @p maxWidth is 0. Defined in utf8.cpp.
 */
std::string truncateVisualFromFront(std::string_view s, std::size_t maxWidth);

/**
 * @brief The longest prefix of @p s that is at most @p maxWidth columns
 *        wide.
 *
 * Cuts on whole character / escape-sequence boundaries only, so a
 * multi-byte character or a CSI sequence is never split. The walk is
 * CSI-aware (displayCharInfoAt), for already-rendered strings that carry
 * escapeNonPrintable() styling; a cut may therefore land between a
 * style-on and its style-off, and a caller that writes the result to the
 * terminal should reset the style when anything was cut.
 *
 * @return A view into @p s, valid as long as @p s is. Defined in utf8.cpp.
 */
std::string_view clipToVisualWidth(std::string_view s, std::size_t maxWidth);

/**
 * @brief Sum of display widths of characters in @c line[from..to).
 *
 * Defined in utf8.cpp.
 */
std::size_t visualColRange(std::string const& line, std::size_t from, std::size_t to);

/**
 * @brief Walk a line from byte offset @p from to find the byte offset
 *        matching a target visual column @p goal.
 *
 * Stops at @p to (or line.size()). Defined in utf8.cpp.
 */
std::size_t offsetForVisualCol(std::string const& line, std::size_t from, std::size_t to,
                               std::size_t goal);

/** @brief Visual-row boundary offsets around a byte offset in a wrapped line. */
struct VisualRowInfo {
  std::size_t prevRowStart;
  std::size_t curRowStart;
  std::size_t nextRowStart;
};

/**
 * @brief Scan a line to find the visual-row boundaries around a byte
 *        offset at a given column width.
 *
 * Defined in utf8.cpp.
 */
VisualRowInfo findVisualRow(std::string const& line, std::size_t byteOff, std::size_t cols);

/**
 * @brief Byte offset at the start of a line's last visual row at a given
 *        column width.
 *
 * Defined in utf8.cpp.
 */
std::size_t lastVisualRowStart(std::string const& line, std::size_t cols);

/**
 * @brief Look up VisualRowInfo from pre-computed row-boundary offsets via
 *        binary search.
 *
 * Defined in utf8.cpp.
 */
VisualRowInfo findVisualRowCached(std::vector<std::size_t> const& rowStarts, std::size_t byteOff,
                                  std::size_t lineSize);

/**
 * @brief Compute visual-row boundary byte offsets for a line at a given
 *        column width.
 *
 * @return A vector whose entries are the byte offsets that begin each
 *         visual row. Defined in utf8.cpp.
 */
std::vector<std::size_t> computeRowBounds(std::string const& line, std::size_t cols);

/**
 * @brief Append the tail of a UTF-8 sequence to @p dest, given a lead byte
 *        the caller has already appended.
 *
 * Bytes are pulled one at a time via @p readByte. On EOF mid-sequence the
 * partial tail is kept (callers may render it as a broken lead). When a
 * non-continuation byte arrives mid-sequence the entire malformed
 * sequence — the lead the caller pre-appended plus any continuation bytes
 * consumed — is removed from @p dest and the offending byte is discarded.
 * This stops a paste like `\\xC2\\x1B[31m` from leaking a raw ESC into a
 * string written verbatim to the terminal: the tail reader cannot become
 * a back door around the caller's "printable bytes only" filter.
 *
 * @tparam ReadByte Callable `bool(char&)` returning true and setting its
 *         out-parameter on success, false on EOF/error.
 * @param lead The lead byte already appended to @p dest.
 * @param dest Destination string.
 * @param readByte The continuation-byte source.
 */
template <typename ReadByte>
void appendUtf8Tail(unsigned char lead, std::string& dest, ReadByte readByte) {
  std::size_t expectedTail = utf8SeqLen(lead) - 1;
  for (std::size_t j = 0; j < expectedTail; ++j) {
    char next;
    if (!readByte(next))
      return;
    if (!isUtf8Continuation(static_cast<unsigned char>(next))) {
      // Drop the partial sequence (lead + j continuation bytes already
      // accepted) and the offending byte. The caller's prior `dest +=
      // lead` left dest.size() at >= 1 + j by construction.
      dest.resize(dest.size() - (1 + j));
      return;
    }
    dest += next;
  }
}

/**
 * @brief Convenience wrapper: read continuation bytes for @p lead from
 *        stdin and append them to @p dest.
 *
 * Same contract as appendUtf8Tail() — malformed sequences are dropped
 * rather than passed through. Defined in utf8.cpp.
 */
void readUtf8Tail(unsigned char lead, std::string& dest);

/**
 * @brief Read a complete UTF-8 code point from stdin given the lead byte.
 *
 * For ASCII (< 0x80) returns the byte directly. For multi-byte sequences,
 * reads the continuation bytes from stdin and decodes. On a read error or
 * EOF mid-sequence the partial buffer is fed to decodeUtf8(), which
 * returns the lead byte with len=1 — so the caller sees the lead byte
 * rather than a malformed code point. Defined in utf8.cpp.
 */
uint32_t readUtf8Codepoint(unsigned char lead);
