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
 * @brief Largest code-point boundary <= @p off in @p s.
 *
 * Steps back over at most three continuation bytes (the longest valid
 * tail), so a run of stray continuation bytes in invalid UTF-8 costs O(1).
 * @pre @p off <= s.size().
 */
inline constexpr std::size_t snapToCodepoint(std::string_view s, std::size_t off) {
  for (int k = 0;
       k < 3 && off > 0 && off < s.size() && isUtf8Continuation(static_cast<unsigned char>(s[off]));
       ++k)
    --off;
  return off;
}

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

/** @brief Inclusive code point range [first, last]. */
struct CodepointRange {
  uint32_t first;
  uint32_t last;
};

/**
 * @brief Emoji that terminals draw two columns wide: the East Asian
 *        Width W ranges of the symbol and emoji blocks (Unicode 16.0).
 *
 * Sorted and non-overlapping. Deliberately left out: the non-emoji W
 * symbols U+2329..232A, U+2630..2637 and U+268A..268F, which many
 * terminals still draw one column wide.
 */
inline constexpr auto kWideEmoji = std::to_array<CodepointRange>({
    {0x231A, 0x231B},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},   {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
    {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},
    {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},   {0x274C, 0x274C},
    {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
    {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},   {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248},
    {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3},
    {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F},
    {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7},
    {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF},
    {0x1FA70, 0x1FA7C}, {0x1FA80, 0x1FA89}, {0x1FA8F, 0x1FAC6}, {0x1FACE, 0x1FADC},
    {0x1FADF, 0x1FAE9}, {0x1FAF0, 0x1FAF8},
});

/**
 * @brief Display width of a Unicode code point.
 *
 * Returns 2 for CJK / wide ranges (Hangul Jamo, CJK radicals,
 * Hiragana/Katakana, CJK unified, Yi, Hangul syllables, fullwidth forms,
 * CJK extensions B-G) and wide emoji (kWideEmoji), and 1 otherwise.
 * Does not handle combining marks or zero-width characters (variation
 * selectors, ZWJ), so an emoji sequence measures as the sum of its parts.
 */
inline constexpr std::size_t codepointWidth(uint32_t cp) {
  if (cp < 0x1100)
    return 1; // fast path: Latin, Greek, Cyrillic, ...
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
  // Binary search for the last range starting at or before cp.
  std::size_t lo = 0, hi = kWideEmoji.size();
  while (lo < hi) {
    auto mid = (lo + hi) / 2;
    if (kWideEmoji[mid].first <= cp)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo > 0 && cp <= kWideEmoji[lo - 1].last ? 2 : 1;
}

/** @brief Width and byte length of the character at @c data[0..avail). */
struct CharInfo {
  std::size_t width; /**< Display columns. */
  std::size_t len;   /**< Bytes consumed. */
};

/**
 * @brief Combined decode + width for the character at @c data[0..avail).
 *
 * Measures a byte exactly as Buffer::render's emitChar draws it: ASCII
 * applies charWidth() rules (tab and control bytes — ESC included — are
 * width 2, rendered "^X"/"^["; printable is 1); a valid multi-byte UTF-8
 * sequence decodes to codepointWidth(); an invalid byte (>= 0x80 with
 * len == 1 from decodeUtf8) gets width 4 for "\\xHH".
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
  auto u = static_cast<unsigned char>(data[0]);
  if (u < 0x80)
    return {charWidth(static_cast<char>(u)), 1};
  auto [cp, len] = decodeUtf8(data, avail);
  if (len == 1)
    return {4, 1}; // invalid byte — displayed as "\xHH"
  return {codepointWidth(cp), len};
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
 * does not start a valid UTF-8 sequence. Printable ASCII and valid
 * multi-byte UTF-8 pass through unchanged. Intended for filenames and
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
