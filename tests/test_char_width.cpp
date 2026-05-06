#define BOOST_TEST_MODULE CharWidth
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "utf8.hpp"

// --- charWidth ---

BOOST_AUTO_TEST_CASE(charWidth_tab) { BOOST_CHECK_EQUAL(charWidth('\t'), 2u); }

BOOST_AUTO_TEST_CASE(charWidth_control_chars) {
  BOOST_CHECK_EQUAL(charWidth('\x01'), 2u); // ^A
  BOOST_CHECK_EQUAL(charWidth('\x1f'), 2u); // ^_
}

BOOST_AUTO_TEST_CASE(charWidth_null) { BOOST_CHECK_EQUAL(charWidth('\0'), 2u); }

BOOST_AUTO_TEST_CASE(charWidth_DEL) { BOOST_CHECK_EQUAL(charWidth('\x7f'), 2u); }

BOOST_AUTO_TEST_CASE(charWidth_printable) {
  BOOST_CHECK_EQUAL(charWidth('A'), 1u);
  BOOST_CHECK_EQUAL(charWidth('z'), 1u);
  BOOST_CHECK_EQUAL(charWidth('~'), 1u);
}

BOOST_AUTO_TEST_CASE(charWidth_space) { BOOST_CHECK_EQUAL(charWidth(' '), 1u); }

// --- utf8SeqLen ---

BOOST_AUTO_TEST_CASE(utf8SeqLen_ascii) {
  BOOST_CHECK_EQUAL(utf8SeqLen(0x41), 1u); // 'A'
  BOOST_CHECK_EQUAL(utf8SeqLen(0x00), 1u);
  BOOST_CHECK_EQUAL(utf8SeqLen(0x7F), 1u);
}

BOOST_AUTO_TEST_CASE(utf8SeqLen_two_byte_lead) {
  BOOST_CHECK_EQUAL(utf8SeqLen(0xC3), 2u); // é lead
}

BOOST_AUTO_TEST_CASE(utf8SeqLen_three_byte_lead) {
  BOOST_CHECK_EQUAL(utf8SeqLen(0xE4), 3u); // CJK lead
}

BOOST_AUTO_TEST_CASE(utf8SeqLen_four_byte_lead) {
  BOOST_CHECK_EQUAL(utf8SeqLen(0xF0), 4u); // emoji lead
}

BOOST_AUTO_TEST_CASE(utf8SeqLen_continuation) {
  BOOST_CHECK_EQUAL(utf8SeqLen(0x80), 1u);
  BOOST_CHECK_EQUAL(utf8SeqLen(0xBF), 1u);
}

BOOST_AUTO_TEST_CASE(utf8SeqLen_invalid_0xFF) { BOOST_CHECK_EQUAL(utf8SeqLen(0xFF), 1u); }

// --- isUtf8Continuation ---

BOOST_AUTO_TEST_CASE(isUtf8Continuation_true) {
  BOOST_CHECK(isUtf8Continuation(0x80));
  BOOST_CHECK(isUtf8Continuation(0xBF));
  BOOST_CHECK(isUtf8Continuation(0xA0));
}

BOOST_AUTO_TEST_CASE(isUtf8Continuation_false) {
  BOOST_CHECK(!isUtf8Continuation(0x41)); // 'A'
  BOOST_CHECK(!isUtf8Continuation(0xC0)); // lead byte
  BOOST_CHECK(!isUtf8Continuation(0x7F)); // DEL
  BOOST_CHECK(!isUtf8Continuation(0x00));
}

// --- decodeUtf8 ---

BOOST_AUTO_TEST_CASE(decodeUtf8_ascii) {
  char data[] = "A";
  auto [cp, len] = decodeUtf8(data, 1);
  BOOST_CHECK_EQUAL(cp, 0x41u);
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_two_byte) {
  // é = U+00E9 = 0xC3 0xA9
  char data[] = "\xC3\xA9";
  auto [cp, len] = decodeUtf8(data, 2);
  BOOST_CHECK_EQUAL(cp, 0x00E9u);
  BOOST_CHECK_EQUAL(len, 2u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_three_byte) {
  // 中 = U+4E2D = 0xE4 0xB8 0xAD
  char data[] = "\xE4\xB8\xAD";
  auto [cp, len] = decodeUtf8(data, 3);
  BOOST_CHECK_EQUAL(cp, 0x4E2Du);
  BOOST_CHECK_EQUAL(len, 3u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_four_byte) {
  // 😀 = U+1F600 = 0xF0 0x9F 0x98 0x80
  char data[] = "\xF0\x9F\x98\x80";
  auto [cp, len] = decodeUtf8(data, 4);
  BOOST_CHECK_EQUAL(cp, 0x1F600u);
  BOOST_CHECK_EQUAL(len, 4u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_truncated) {
  // 3-byte lead but only 1 byte available
  char data[] = "\xE4";
  auto [cp, len] = decodeUtf8(data, 1);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xE4u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_overlong_2byte) {
  // C0 80 would decode to U+0000 — overlong
  char data[] = "\xC0\x80";
  auto [cp, len] = decodeUtf8(data, 2);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xC0u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_overlong_3byte) {
  // E0 80 80 would decode to U+0000 — overlong
  char data[] = "\xE0\x80\x80";
  auto [cp, len] = decodeUtf8(data, 3);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xE0u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_overlong_4byte) {
  // F0 80 80 80 would decode to U+0000 — overlong
  char data[] = "\xF0\x80\x80\x80";
  auto [cp, len] = decodeUtf8(data, 4);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xF0u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_surrogate) {
  // ED A0 80 = U+D800 — surrogate
  char data[] = "\xED\xA0\x80";
  auto [cp, len] = decodeUtf8(data, 3);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xEDu));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_out_of_range) {
  // F4 90 80 80 = U+110000 — beyond Unicode
  char data[] = "\xF4\x90\x80\x80";
  auto [cp, len] = decodeUtf8(data, 4);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xF4u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_invalid_continuation) {
  // C3 followed by non-continuation byte
  char data[] = "\xC3\x41";
  auto [cp, len] = decodeUtf8(data, 2);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0xC3u));
  BOOST_CHECK_EQUAL(len, 1u);
}

BOOST_AUTO_TEST_CASE(decodeUtf8_stray_continuation) {
  // Lone continuation byte
  char data[] = "\x80";
  auto [cp, len] = decodeUtf8(data, 1);
  BOOST_CHECK_EQUAL(cp, static_cast<uint32_t>(0x80u));
  BOOST_CHECK_EQUAL(len, 1u);
}

// --- encodeUtf8 ---

BOOST_AUTO_TEST_CASE(encodeUtf8_ascii) {
  auto [len, buf] = encodeUtf8(0x41); // 'A'
  BOOST_CHECK_EQUAL(len, 1u);
  BOOST_CHECK_EQUAL(buf[0], 0x41u);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_two_byte) {
  auto [len, buf] = encodeUtf8(0x00E9); // 'é'
  BOOST_CHECK_EQUAL(len, 2u);
  BOOST_CHECK_EQUAL(buf[0], 0xC3u);
  BOOST_CHECK_EQUAL(buf[1], 0xA9u);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_three_byte) {
  auto [len, buf] = encodeUtf8(0x4E2D); // '中'
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xE4u);
  BOOST_CHECK_EQUAL(buf[1], 0xB8u);
  BOOST_CHECK_EQUAL(buf[2], 0xADu);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_four_byte) {
  auto [len, buf] = encodeUtf8(0x1F600); // emoji
  BOOST_CHECK_EQUAL(len, 4u);
  BOOST_CHECK_EQUAL(buf[0], 0xF0u);
  BOOST_CHECK_EQUAL(buf[1], 0x9Fu);
  BOOST_CHECK_EQUAL(buf[2], 0x98u);
  BOOST_CHECK_EQUAL(buf[3], 0x80u);
}

// Surrogate halves (U+D800..U+DFFF) are not valid Unicode scalar
// values. encodeUtf8 must substitute U+FFFD rather than silently
// producing an ill-formed 3-byte sequence.
BOOST_AUTO_TEST_CASE(encodeUtf8_surrogate_low_replaced_with_fffd) {
  auto [len, buf] = encodeUtf8(0xD800);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEFu);
  BOOST_CHECK_EQUAL(buf[1], 0xBFu);
  BOOST_CHECK_EQUAL(buf[2], 0xBDu);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_surrogate_mid_replaced_with_fffd) {
  auto [len, buf] = encodeUtf8(0xDABC);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEFu);
  BOOST_CHECK_EQUAL(buf[1], 0xBFu);
  BOOST_CHECK_EQUAL(buf[2], 0xBDu);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_surrogate_high_replaced_with_fffd) {
  auto [len, buf] = encodeUtf8(0xDFFF);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEFu);
  BOOST_CHECK_EQUAL(buf[1], 0xBFu);
  BOOST_CHECK_EQUAL(buf[2], 0xBDu);
}

// Codepoints just outside the surrogate range should still encode
// normally.
BOOST_AUTO_TEST_CASE(encodeUtf8_before_surrogate_range_ok) {
  auto [len, buf] = encodeUtf8(0xD7FF);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEDu);
  BOOST_CHECK_EQUAL(buf[1], 0x9Fu);
  BOOST_CHECK_EQUAL(buf[2], 0xBFu);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_after_surrogate_range_ok) {
  auto [len, buf] = encodeUtf8(0xE000);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEEu);
  BOOST_CHECK_EQUAL(buf[1], 0x80u);
  BOOST_CHECK_EQUAL(buf[2], 0x80u);
}

BOOST_AUTO_TEST_CASE(encodeUtf8_out_of_range_replaced_with_fffd) {
  auto [len, buf] = encodeUtf8(0x110000);
  BOOST_CHECK_EQUAL(len, 3u);
  BOOST_CHECK_EQUAL(buf[0], 0xEFu);
  BOOST_CHECK_EQUAL(buf[1], 0xBFu);
  BOOST_CHECK_EQUAL(buf[2], 0xBDu);
}

// --- codepointWidth ---

BOOST_AUTO_TEST_CASE(codepointWidth_ascii) {
  BOOST_CHECK_EQUAL(codepointWidth(0x41), 1u); // 'A'
}

BOOST_AUTO_TEST_CASE(codepointWidth_cjk_unified) {
  BOOST_CHECK_EQUAL(codepointWidth(0x4E2D), 2u); // 中
}

BOOST_AUTO_TEST_CASE(codepointWidth_hangul_syllable) {
  BOOST_CHECK_EQUAL(codepointWidth(0xAC00), 2u); // 가
}

BOOST_AUTO_TEST_CASE(codepointWidth_fullwidth) {
  BOOST_CHECK_EQUAL(codepointWidth(0xFF01), 2u); // ！
}

BOOST_AUTO_TEST_CASE(codepointWidth_latin_supplement) {
  BOOST_CHECK_EQUAL(codepointWidth(0x00E9), 1u); // é
}

// --- charInfoAt ---

BOOST_AUTO_TEST_CASE(charInfoAt_printable_ascii) {
  char data[] = "A";
  auto info = charInfoAt(data, 1);
  BOOST_CHECK_EQUAL(info.width, 1u);
  BOOST_CHECK_EQUAL(info.len, 1u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_tab) {
  char data[] = "\t";
  auto info = charInfoAt(data, 1);
  BOOST_CHECK_EQUAL(info.width, 2u);
  BOOST_CHECK_EQUAL(info.len, 1u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_control) {
  char data[] = "\x01";
  auto info = charInfoAt(data, 1);
  BOOST_CHECK_EQUAL(info.width, 2u);
  BOOST_CHECK_EQUAL(info.len, 1u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_cjk) {
  // 中 = 3 bytes, width 2
  char data[] = "\xE4\xB8\xAD";
  auto info = charInfoAt(data, 3);
  BOOST_CHECK_EQUAL(info.width, 2u);
  BOOST_CHECK_EQUAL(info.len, 3u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_invalid_byte) {
  // Stray continuation byte — rendered as "\xHH", width 4, len 1
  char data[] = "\x80";
  auto info = charInfoAt(data, 1);
  BOOST_CHECK_EQUAL(info.width, 4u);
  BOOST_CHECK_EQUAL(info.len, 1u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_overlong) {
  // C0 80 — overlong, first byte treated as invalid ("\xc0", width 4)
  char data[] = "\xC0\x80";
  auto info = charInfoAt(data, 2);
  BOOST_CHECK_EQUAL(info.width, 4u);
  BOOST_CHECK_EQUAL(info.len, 1u);
}

// --- truncateVisualFromFront ---

BOOST_AUTO_TEST_CASE(truncateFromFront_fits) {
  // No truncation needed when the string already fits.
  BOOST_CHECK_EQUAL(truncateVisualFromFront("hello", 10), "hello");
  BOOST_CHECK_EQUAL(truncateVisualFromFront("hello", 5), "hello");
  BOOST_CHECK_EQUAL(truncateVisualFromFront("", 5), "");
}

BOOST_AUTO_TEST_CASE(truncateFromFront_ascii) {
  // "abcdef" (width 6) truncated to 4 => "…def" (ellipsis width 1 + 3 chars).
  auto r = truncateVisualFromFront("abcdef", 4);
  BOOST_CHECK_EQUAL(r, "\xe2\x80\xa6"
                       "def");
  BOOST_CHECK_EQUAL(visualWidth(r), 4u);
}

BOOST_AUTO_TEST_CASE(truncateFromFront_keeps_unique_tail) {
  // A long path should be truncated from the front, preserving the filename
  // (the rightmost component is the most unique).
  auto r = truncateVisualFromFront("/very/long/path/to/buffer_view.cpp", 20);
  BOOST_CHECK(visualWidth(r) <= 20u);
  BOOST_CHECK(r.find("buffer_view.cpp") != std::string::npos);
  BOOST_CHECK_EQUAL(r.substr(0, 3), "\xe2\x80\xa6");
}

BOOST_AUTO_TEST_CASE(truncateFromFront_zero_width) {
  BOOST_CHECK_EQUAL(truncateVisualFromFront("hello", 0), "");
}

BOOST_AUTO_TEST_CASE(truncateFromFront_cjk) {
  // Each CJK char is width 2. "中中中中" is width 8.
  std::string s = "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad"; // 中中中中
  auto r = truncateVisualFromFront(s, 5);
  // Available tail budget after ellipsis: 4, fits exactly 2 CJK chars.
  BOOST_CHECK(visualWidth(r) <= 5u);
  // Leading ellipsis.
  BOOST_CHECK_EQUAL(r.substr(0, 3), "\xe2\x80\xa6");
}

// --- escapeNonPrintable ---
//
// Output wraps each escaped byte (or contiguous run of escaped bytes) in
// ansi::CtrlColor / ansi::Reset so it matches the buffer text view's
// styling for control bytes and broken UTF-8. Tests spell those out via
// constants so the expected strings stay readable.

namespace {
constexpr std::string_view kStyleOn = "\x1b[38;2;204;120;50m";
constexpr std::string_view kStyleOff = "\x1b[39m"; // DefaultFg, not full Reset
std::string styled(std::string_view escaped) {
  return std::string(kStyleOn) + std::string(escaped) + std::string(kStyleOff);
}
} // namespace

BOOST_AUTO_TEST_CASE(escapeNonPrintable_passthrough) {
  BOOST_CHECK_EQUAL(escapeNonPrintable(""), "");
  BOOST_CHECK_EQUAL(escapeNonPrintable("hello.txt"), "hello.txt");
  BOOST_CHECK_EQUAL(escapeNonPrintable("/path/to/file"), "/path/to/file");
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_del_and_backspace) {
  BOOST_CHECK_EQUAL(escapeNonPrintable("\x7f."), styled("\\x7f") + ".");
  // Split string literal so `f` does not get pulled into the \x escape.
  BOOST_CHECK_EQUAL(escapeNonPrintable("\x08"
                                       "foo"),
                    styled("\\x08") + "foo");
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_low_controls) {
  BOOST_CHECK_EQUAL(escapeNonPrintable(std::string("a\0b", 3)), "a" + styled("\\x00") + "b");
  BOOST_CHECK_EQUAL(escapeNonPrintable("\nfoo\r"), styled("\\x0a") + "foo" + styled("\\x0d"));
  // TAB is escaped: it would shift columns and corrupt alignment.
  BOOST_CHECK_EQUAL(escapeNonPrintable("a\tb"), "a" + styled("\\x09") + "b");
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_runs_coalesce) {
  // Adjacent escaped bytes share one CtrlColor / Reset pair instead of
  // emitting CSI per byte.
  BOOST_CHECK_EQUAL(escapeNonPrintable(std::string("\x01\x02", 2)), styled("\\x01\\x02"));
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_valid_utf8_passes_through) {
  // 中 = E4 B8 AD. Valid 3-byte sequence, should not be escaped.
  std::string cjk = "\xe4\xb8\xad";
  BOOST_CHECK_EQUAL(escapeNonPrintable(cjk), cjk);
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_invalid_high_bytes) {
  // 0xFF is never a valid UTF-8 lead byte.
  BOOST_CHECK_EQUAL(escapeNonPrintable("\xff"), styled("\\xff"));
  // Lone continuation byte 0x80 is also invalid.
  BOOST_CHECK_EQUAL(escapeNonPrintable("a\x80z"), "a" + styled("\\x80") + "z");
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_truncated_multibyte) {
  // 0xE4 introduces a 3-byte sequence but is followed by ASCII —
  // decodeUtf8 should reject it; the lead byte gets escaped.
  BOOST_CHECK_EQUAL(escapeNonPrintable("\xe4z"), styled("\\xe4") + "z");
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_visualWidth_unaffected_by_styling) {
  // Inline CSI sequences are zero-width to visualWidth, so callers can
  // measure the result for layout without subtracting the styling.
  BOOST_CHECK_EQUAL(visualWidth(escapeNonPrintable("\x7f.")), 5u); // "\x7f" (4) + "." (1)
  BOOST_CHECK_EQUAL(visualWidth(escapeNonPrintable("hello")), 5u);
}
