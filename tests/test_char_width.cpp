#define BOOST_TEST_MODULE CharWidth
#include <boost/test/unit_test.hpp>

#include "byte_constants.hpp"
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

// Expected widths below were measured in xfce4-terminal (VTE 0.84), the
// reference terminal, by querying the cursor position after each one.

BOOST_AUTO_TEST_CASE(codepointWidth_emoji_is_wide) {
  BOOST_CHECK_EQUAL(codepointWidth(0x1F600), 2u); // 😀
  BOOST_CHECK_EQUAL(codepointWidth(0x1F44B), 2u); // 👋
  BOOST_CHECK_EQUAL(codepointWidth(0x1F680), 2u); // 🚀
  BOOST_CHECK_EQUAL(codepointWidth(0x2705), 2u);  // ✅
  BOOST_CHECK_EQUAL(codepointWidth(0x1F3FD), 2u); // skin tone modifier on its own
  BOOST_CHECK_EQUAL(codepointWidth(0x1F972), 2u); // Unicode 13
  BOOST_CHECK_EQUAL(codepointWidth(0x1FAE0), 2u); // Unicode 14
  BOOST_CHECK_EQUAL(codepointWidth(0x1FAE8), 2u); // Unicode 15
  BOOST_CHECK_EQUAL(visualWidth("a\xF0\x9F\x91\x8B\xF0\x9F\x99\x82"
                                "b"),
                    6u); // a👋🙂b
}

BOOST_AUTO_TEST_CASE(codepointWidth_follows_unicode_15_1) {
  BOOST_CHECK_EQUAL(codepointWidth(0x2329), 2u);  // 〈
  BOOST_CHECK_EQUAL(codepointWidth(0x33C0), 2u);  // ㏀ CJK compatibility
  BOOST_CHECK_EQUAL(codepointWidth(0x1B000), 2u); // 𛀀 Kana Supplement
  // Wide only since Unicode 16.0: narrow in the reference terminal.
  BOOST_CHECK_EQUAL(codepointWidth(0x1FAE9), 1u);
  BOOST_CHECK_EQUAL(codepointWidth(0x2630), 1u); // ☰
  BOOST_CHECK_EQUAL(codepointWidth(0x268A), 1u);
}

BOOST_AUTO_TEST_CASE(codepointWidth_text_symbols_stay_narrow) {
  BOOST_CHECK_EQUAL(codepointWidth(0x2764), 1u);  // ❤ (text presentation by default)
  BOOST_CHECK_EQUAL(codepointWidth(0x263A), 1u);  // ☺
  BOOST_CHECK_EQUAL(codepointWidth(0x1F321), 1u); // 🌡
  BOOST_CHECK_EQUAL(codepointWidth(0x1F1E9), 1u); // regional indicator: a flag pair is 2
  BOOST_CHECK_EQUAL(codepointWidth(0x2460), 1u);  // ① East Asian ambiguous
}

BOOST_AUTO_TEST_CASE(unicode_table_is_sorted_and_disjoint) {
  auto const& t = kCodepointClasses;
  for (std::size_t i = 0; i < t.size(); ++i) {
    BOOST_CHECK_LE(t[i].first, t[i].last);
    BOOST_CHECK(t[i].cls != CodepointClass::Normal);
    if (i > 0)
      BOOST_CHECK_LT(t[i - 1].last, t[i].first);
  }
}

BOOST_AUTO_TEST_CASE(codepointClass_fast_paths_agree_with_table) {
  // Walk the table alongside every code point and compare.
  std::size_t k = 0, mismatches = 0;
  for (uint32_t cp = 0; cp <= kMaxCodepoint; ++cp) {
    while (k < kCodepointClasses.size() && kCodepointClasses[k].last < cp)
      ++k;
    auto expected = k < kCodepointClasses.size() && kCodepointClasses[k].first <= cp
                        ? kCodepointClasses[k].cls
                        : CodepointClass::Normal;
    mismatches += codepointClass(cp) != expected;
  }
  BOOST_CHECK_EQUAL(mismatches, 0u);
}

BOOST_AUTO_TEST_CASE(c1_controls_and_separators_are_escaped) {
  // U+009B is CSI to a terminal: it must never be sent raw.
  std::string_view csi = "\xC2\x9B"
                         "31m";
  auto info = charInfoAt(csi.data(), csi.size());
  BOOST_CHECK_EQUAL(info.width, 6u);
  BOOST_CHECK_EQUAL(info.len, 2u);
  BOOST_CHECK(escapeNonPrintable(csi).find("\xC2\x9B") == std::string::npos);
  BOOST_CHECK(escapeNonPrintable(csi).find("\\u009b") != std::string::npos);
  BOOST_CHECK_EQUAL(visualWidth("\xC2\x80"), 6u);     // U+0080
  BOOST_CHECK_EQUAL(visualWidth("\xC2\x9F"), 6u);     // U+009F
  BOOST_CHECK_EQUAL(visualWidth("\xC2\xA0"), 1u);     // NBSP stays a character
  BOOST_CHECK_EQUAL(visualWidth("\xE2\x80\xA8"), 6u); // U+2028 line separator
  BOOST_CHECK_EQUAL(visualWidth("\xE2\x80\xA9"), 6u); // U+2029 paragraph separator
}

// --- Clusters, zero-width marks and format characters ---

namespace {

struct MeasuredCase {
  char const* name;
  char const* text;
  std::size_t terminalWidth; // cursor advance measured in xfce4-terminal
  std::size_t cxWidth;       // differs only where cx escapes a format character
};

MeasuredCase const kMeasured[] = {
    {"ASCII A", "A", 1, 1},
    {"e-acute precomposed", "\xC3\xA9", 1, 1},
    {"e + combining acute", "e\xCC\x81", 1, 1},
    {"a + 3 combining marks", "a\xCC\x81\xCC\x88\xCC\xA3", 1, 1},
    {"CJK", "\xE4\xB8\xAD", 2, 2},
    {"grinning face 1F600", "\xF0\x9F\x98\x80", 2, 2},
    {"waving hand 1F44B", "\xF0\x9F\x91\x8B", 2, 2},
    {"rocket 1F680", "\xF0\x9F\x9A\x80", 2, 2},
    {"check 2705", "\xE2\x9C\x85", 2, 2},
    {"heart 2764 (text default)", "\xE2\x9D\xA4", 1, 1},
    {"heart + VS16 FE0F", "\xE2\x9D\xA4\xEF\xB8\x8F", 1, 1},
    {"smiley 263A", "\xE2\x98\xBA", 1, 1},
    {"smiley 263A + VS16", "\xE2\x98\xBA\xEF\xB8\x8F", 1, 1},
    {"grinning + VS15 FE0E", "\xF0\x9F\x98\x80\xEF\xB8\x8E", 2, 2},
    {"trigram 2630 (U16 wide)", "\xE2\x98\xB0", 1, 1},
    {"monogram 268A (U16 wide)", "\xE2\x9A\x8A", 1, 1},
    {"angle bracket 2329", "\xE2\x8C\xA9", 2, 2},
    {"hand + skin tone 1F3FD", "\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD", 4, 4},
    {"skin tone alone 1F3FD", "\xF0\x9F\x8F\xBD", 2, 2},
    {"family ZWJ sequence",
     "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7", 6, 18},
    {"rainbow flag ZWJ", "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88", 3, 9},
    {"flag DE (2 regional ind.)", "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA", 2, 2},
    {"regional indicator alone", "\xF0\x9F\x87\xA9", 1, 1},
    {"keycap 1 FE0F 20E3", "1\xEF\xB8\x8F\xE2\x83\xA3", 1, 1},
    {"ZWSP 200B between x", "x\xE2\x80\x8Bx", 2, 8},
    {"ZWNJ 200C between x", "x\xE2\x80\x8Cx", 2, 8},
    {"ZWJ 200D between x", "x\xE2\x80\x8Dx", 2, 8},
    {"word joiner 2060 betw x", "x\xE2\x81\xA0x", 2, 8},
    {"BOM FEFF between x", "x\xEF\xBB\xBFx", 2, 8},
    {"soft hyphen 00AD betw x", "x\xC2\xADx", 3, 8},
    {"LRM 200E between x", "x\xE2\x80\x8Ex", 2, 8},
    {"Hangul jamo L+V 1100 1161", "\xE1\x84\x80\xE1\x85\xA1", 2, 2},
    {"Thai ko + sara i", "\xE0\xB8\x81\xE0\xB8\xB4", 1, 1},
    {"Devanagari namaste",
     "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87", 4, 4},
    {"Hebrew with point", "\xD7\xA9\xD7\x81", 1, 1},
    {"emoji 13 1F972", "\xF0\x9F\xA5\xB2", 2, 2},
    {"emoji 14 1FAE0", "\xF0\x9F\xAB\xA0", 2, 2},
    {"emoji 15 1FAE8", "\xF0\x9F\xAB\xA8", 2, 2},
    {"emoji 16 1FAE9", "\xF0\x9F\xAB\xA9", 1, 1},
    {"thermometer 1F321", "\xF0\x9F\x8C\xA1", 1, 1},
    {"thermometer + VS16", "\xF0\x9F\x8C\xA1\xEF\xB8\x8F", 1, 1},
    {"circled 1 2460 (ambig.)", "\xE2\x91\xA0", 1, 1},
};

} // namespace

BOOST_AUTO_TEST_CASE(visualWidth_matches_reference_terminal) {
  for (auto const& c : kMeasured) {
    BOOST_TEST_CONTEXT(c.name) { BOOST_CHECK_EQUAL(visualWidth(c.text), c.cxWidth); }
  }
}

BOOST_AUTO_TEST_CASE(charInfoAt_absorbs_combining_marks) {
  std::string_view s = "e\xCC\x81x"; // e + U+0301, x
  auto info = charInfoAt(s.data(), s.size());
  BOOST_CHECK_EQUAL(info.width, 1u);
  BOOST_CHECK_EQUAL(info.len, 3u);

  std::string_view heart = "\xE2\x9D\xA4\xEF\xB8\x8F"; // U+2764 U+FE0F
  info = charInfoAt(heart.data(), heart.size());
  BOOST_CHECK_EQUAL(info.width, 1u);
  BOOST_CHECK_EQUAL(info.len, 6u);

  std::string_view hand = "\xF0\x9F\x91\x8B\xF0\x9F\x8F\xBD"; // 👋 + skin tone: two clusters
  info = charInfoAt(hand.data(), hand.size());
  BOOST_CHECK_EQUAL(info.width, 2u);
  BOOST_CHECK_EQUAL(info.len, 4u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_orphan_marks_are_one_column) {
  std::string_view s = "\xCC\x81\xCC\x88"
                       "a"; // two marks at line start
  auto info = charInfoAt(s.data(), s.size());
  BOOST_CHECK_EQUAL(info.width, 1u);
  BOOST_CHECK_EQUAL(info.len, 4u);

  std::string_view tab = "\t\xCC\x81"; // a tab doesn't take marks
  info = charInfoAt(tab.data(), tab.size());
  BOOST_CHECK_EQUAL(info.len, 1u);
  BOOST_CHECK_EQUAL(visualWidth(tab), 3u);
}

BOOST_AUTO_TEST_CASE(charInfoAt_format_chars_are_escapes) {
  std::string_view zwsp = "\xE2\x80\x8B\xCC\x81"; // ZWSP, then an (orphan) mark
  auto info = charInfoAt(zwsp.data(), zwsp.size());
  BOOST_CHECK_EQUAL(info.width, 6u);
  BOOST_CHECK_EQUAL(info.len, 3u);
  BOOST_CHECK_EQUAL(visualWidth("\xF3\xA0\x80\x81"), 9u); // U+E0001 "\u{e0001}"
  BOOST_CHECK_EQUAL(codepointEscape(0x200B), "\\u200b");
  BOOST_CHECK_EQUAL(codepointEscape(0xAD), "\\u00ad");
  BOOST_CHECK_EQUAL(codepointEscape(0xE0001), "\\u{e0001}");
}

BOOST_AUTO_TEST_CASE(cluster_boundaries) {
  std::string_view s = "ae\xCC\x81\xCC\x88"
                       "b"; // a, e+2 marks (bytes 1..5), b at 6
  BOOST_CHECK_EQUAL(clusterStart(s, 0), 0u);
  BOOST_CHECK_EQUAL(clusterStart(s, 1), 1u);
  BOOST_CHECK_EQUAL(clusterStart(s, 3), 1u); // on the first mark
  BOOST_CHECK_EQUAL(clusterStart(s, 4), 1u); // mid-codepoint inside the second mark
  BOOST_CHECK_EQUAL(clusterStart(s, 6), 6u);
  BOOST_CHECK_EQUAL(clusterStart(s, 7), 7u); // end of string
  BOOST_CHECK_EQUAL(prevClusterStart(s, 6), 1u);
  BOOST_CHECK_EQUAL(prevClusterStart(s, 1), 0u);

  std::string_view orphan = "\t\xCC\x81\xCC\x88x"; // tab, two orphan marks, x
  BOOST_CHECK_EQUAL(clusterStart(orphan, 3), 1u);
  BOOST_CHECK_EQUAL(prevClusterStart(orphan, 5), 1u);
  BOOST_CHECK_EQUAL(prevClusterStart(orphan, 1), 0u);
}

BOOST_AUTO_TEST_CASE(escapeNonPrintable_format_and_orphan) {
  BOOST_CHECK_EQUAL(escapeNonPrintable("a\xE2\x80\x8B"
                                       "b"),
                    std::string("a") + std::string(ansi::CtrlColor) + "\\u200b" +
                        std::string(ansi::DefaultFg) + "b");
  BOOST_CHECK_EQUAL(escapeNonPrintable("\xCC\x81"), "\xE2\x97\x8C\xCC\x81");
  BOOST_CHECK_EQUAL(escapeNonPrintable("e\xCC\x81"), "e\xCC\x81");
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

BOOST_AUTO_TEST_CASE(charInfoAt_escape_is_a_control_byte) {
  // On raw buffer content a bare ESC is a control byte: width 2 ("^[")
  // and length 1, matching how Buffer::render's emitChar draws it.
  char esc[] = "\x1b";
  auto info = charInfoAt(esc, 1);
  BOOST_CHECK_EQUAL(info.width, 2u);
  BOOST_CHECK_EQUAL(info.len, 1u);

  // "ESC[31m" as content: charInfoAt must NOT swallow it as a zero-width
  // CSI run. It sees only the ESC (the "[31m" are ordinary printables),
  // so it advances one byte at a time — which is what keeps a file full
  // of ANSI escapes from dropping bytes and desyncing the cursor column.
  char csi[] = "\x1b[31m";
  auto first = charInfoAt(csi, 5);
  BOOST_CHECK_EQUAL(first.width, 2u);
  BOOST_CHECK_EQUAL(first.len, 1u);
}

BOOST_AUTO_TEST_CASE(displayCharInfoAt_skips_csi) {
  // On already-rendered strings (modeline / escapeNonPrintable output) a
  // CSI escape is a zero-width, full-length pass-through.
  char csi[] = "\x1b[31m";
  auto info = displayCharInfoAt(csi, 5);
  BOOST_CHECK_EQUAL(info.width, 0u);
  BOOST_CHECK_EQUAL(info.len, 5u);

  // Non-CSI bytes fall through to charInfoAt's content semantics.
  char a[] = "A";
  auto plain = displayCharInfoAt(a, 1);
  BOOST_CHECK_EQUAL(plain.width, 1u);
  BOOST_CHECK_EQUAL(plain.len, 1u);

  // A lone ESC not followed by '[' is still a control byte.
  char esc[] = "\x1bX";
  auto lone = displayCharInfoAt(esc, 2);
  BOOST_CHECK_EQUAL(lone.width, 2u);
  BOOST_CHECK_EQUAL(lone.len, 1u);
}

// Content-width helpers must count an embedded ANSI escape as the visible
// "^[" plus its now-ordinary trailing bytes, mirroring what the renderer
// paints — regression for the charInfoAt/emitChar mismatch.
BOOST_AUTO_TEST_CASE(visualColRange_counts_escape_as_content) {
  std::string line = "\x1b[31mX"; // ESC [ 3 1 m X
  // "^[" (2) + '[' + '3' + '1' + 'm' (1 each) + 'X' (1) = 7 columns.
  BOOST_CHECK_EQUAL(visualColRange(line, 0, line.size()), 7u);
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

// --- appendUtf8Tail ---
//
// The interesting cases are around byte validation: a non-continuation
// byte (anything outside 0x80..0xBF) signals a malformed sequence and
// must drop the partial sequence rather than letting the offending
// byte (e.g. a raw ESC, BEL) flow through to a place that writes its
// input verbatim to the terminal.

namespace {
// A reader callback backed by a byte queue, for driving appendUtf8Tail
// from tests without touching stdin.
struct ByteQueue {
  std::string data;
  std::size_t pos = 0;
  bool operator()(char& out) {
    if (pos >= data.size())
      return false;
    out = data[pos++];
    return true;
  }
};
} // namespace

BOOST_AUTO_TEST_CASE(appendUtf8Tail_complete_2byte) {
  // 'é' = U+00E9 = 0xC3 0xA9. Lead already in dest; tail provides 0xA9.
  std::string dest = "\xC3";
  ByteQueue src{"\xA9"};
  appendUtf8Tail(0xC3, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "\xC3\xA9");
  BOOST_CHECK_EQUAL(src.pos, 1u);
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_complete_3byte) {
  // '中' = 0xE4 0xB8 0xAD.
  std::string dest = "\xE4";
  ByteQueue src{"\xB8\xAD"};
  appendUtf8Tail(0xE4, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "\xE4\xB8\xAD");
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_complete_4byte) {
  // Emoji: U+1F600 = 0xF0 0x9F 0x98 0x80.
  std::string dest = "\xF0";
  ByteQueue src{"\x9F\x98\x80"};
  appendUtf8Tail(0xF0, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "\xF0\x9F\x98\x80");
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_eof_keeps_partial) {
  // EOF mid-sequence: keep what we have (the lead the caller appended).
  std::string dest = "prefix\xE4";
  ByteQueue src{""};
  appendUtf8Tail(0xE4, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "prefix\xE4");
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_eof_after_one_continuation) {
  // 3-byte sequence, only one continuation byte available.
  std::string dest = "\xE4";
  ByteQueue src{"\xB8"};
  appendUtf8Tail(0xE4, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "\xE4\xB8");
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_rejects_esc_after_lead) {
  // The regression case: 0xC2 followed by 0x1B. Old behaviour appended
  // 0x1B (a raw escape) into dest; new behaviour drops the lead and the
  // offending byte.
  std::string dest = "prefix\xC2";
  ByteQueue src{"\x1B[31m"};
  appendUtf8Tail(0xC2, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "prefix");
  // Only the offending byte was consumed — the rest of the source
  // remains available for the outer reader.
  BOOST_CHECK_EQUAL(src.pos, 1u);
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_rejects_bel_after_lead) {
  // BEL (0x07) terminates OSC sequences. The same path must not
  // smuggle one in.
  std::string dest = "\xC2";
  ByteQueue src{"\x07"};
  appendUtf8Tail(0xC2, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "");
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_rejects_ascii_in_continuation_slot) {
  // 4-byte lead with a printable ASCII byte where the first continuation
  // would go. Drop the lead — the ASCII byte is not a continuation byte
  // even though it's safe to print.
  std::string dest = "\xF0";
  ByteQueue src{"abc"};
  appendUtf8Tail(0xF0, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "");
  BOOST_CHECK_EQUAL(src.pos, 1u);
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_rejects_in_second_continuation) {
  // 4-byte lead. Two valid continuation bytes, then an ESC where the
  // third would go. Drop everything we accepted (lead + 2 bytes) and
  // the offending byte.
  std::string dest = "x\xF0";
  ByteQueue src{"\x9F\x98\x1B"};
  appendUtf8Tail(0xF0, dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "x");
  BOOST_CHECK_EQUAL(src.pos, 3u);
}

BOOST_AUTO_TEST_CASE(appendUtf8Tail_ascii_lead_is_noop) {
  // Lead < 0x80 means a 1-byte sequence — no tail to read at all.
  std::string dest = "A";
  ByteQueue src{"BCDE"};
  appendUtf8Tail('A', dest, std::ref(src));
  BOOST_CHECK_EQUAL(dest, "A");
  BOOST_CHECK_EQUAL(src.pos, 0u);
}

// --- snapToCodepoint ---

BOOST_AUTO_TEST_CASE(snap_to_codepoint_backs_up_to_lead_byte) {
  std::string s = "a\xe4\xb8\xad"
                  "b"; // a 中 b
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 0), 0u);
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 1), 1u);
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 2), 1u);
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 3), 1u);
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 4), 4u);
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 5), 5u); // end of string
}

BOOST_AUTO_TEST_CASE(snap_to_codepoint_bounded_on_stray_continuations) {
  std::string s(10, '\x80');
  BOOST_CHECK_EQUAL(snapToCodepoint(s, 8), 5u);
}

// --- Characters wider than the wrap width ---

BOOST_AUTO_TEST_CASE(row_bounds_no_empty_rows_for_too_wide_chars) {
  // At a row start a character wider than cols used to open a new row
  // first, so cols=1 "中文字" gave 0 0 3 3 6 6 9.
  std::string s = "\xe4\xb8\xad\xe6\x96\x87\xe5\xad\x97";
  BOOST_CHECK(computeRowBounds(s, 1) == (std::vector<std::size_t>{0, 3, 6, 9}));
  // Row bounds stay strictly increasing for any width.
  for (std::size_t cols = 1; cols <= 4; ++cols) {
    auto rb = computeRowBounds(s, cols);
    for (std::size_t i = 1; i < rb.size(); ++i)
      BOOST_CHECK_LT(rb[i - 1], rb[i]);
  }
}

BOOST_AUTO_TEST_CASE(visual_row_helpers_agree_with_row_bounds_for_wide_chars) {
  std::string s = "a\xe4\xb8\xad"
                  "b";
  auto rb = computeRowBounds(s, 1);
  BOOST_CHECK(rb == (std::vector<std::size_t>{0, 1, 4, 5}));
  BOOST_CHECK_EQUAL(lastVisualRowStart(s, 1), rb.back());
  auto info = findVisualRow(s, 1, 1);
  BOOST_CHECK_EQUAL(info.curRowStart, 1u);
  BOOST_CHECK_EQUAL(info.nextRowStart, 4u);
}
