#define BOOST_TEST_MODULE CharWidth
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/char_width.hpp"

// --- charWidth ---

BOOST_AUTO_TEST_CASE(charWidth_tab) {
    BOOST_CHECK_EQUAL(charWidth('\t'), 2u);
}

BOOST_AUTO_TEST_CASE(charWidth_control_chars) {
    BOOST_CHECK_EQUAL(charWidth('\x01'), 2u); // ^A
    BOOST_CHECK_EQUAL(charWidth('\x1f'), 2u); // ^_
}

BOOST_AUTO_TEST_CASE(charWidth_null) {
    BOOST_CHECK_EQUAL(charWidth('\0'), 2u);
}

BOOST_AUTO_TEST_CASE(charWidth_DEL) {
    BOOST_CHECK_EQUAL(charWidth('\x7f'), 2u);
}

BOOST_AUTO_TEST_CASE(charWidth_printable) {
    BOOST_CHECK_EQUAL(charWidth('A'), 1u);
    BOOST_CHECK_EQUAL(charWidth('z'), 1u);
    BOOST_CHECK_EQUAL(charWidth('~'), 1u);
}

BOOST_AUTO_TEST_CASE(charWidth_space) {
    BOOST_CHECK_EQUAL(charWidth(' '), 1u);
}

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

BOOST_AUTO_TEST_CASE(utf8SeqLen_invalid_0xFF) {
    BOOST_CHECK_EQUAL(utf8SeqLen(0xFF), 1u);
}

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
    // Stray continuation byte — width 2, len 1
    char data[] = "\x80";
    auto info = charInfoAt(data, 1);
    BOOST_CHECK_EQUAL(info.width, 2u);
    BOOST_CHECK_EQUAL(info.len, 1u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_overlong) {
    // C0 80 — overlong, first byte treated as invalid
    char data[] = "\xC0\x80";
    auto info = charInfoAt(data, 2);
    BOOST_CHECK_EQUAL(info.width, 2u);
    BOOST_CHECK_EQUAL(info.len, 1u);
}
