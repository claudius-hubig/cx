#define BOOST_TEST_MODULE PromptHistory
#include <boost/test/unit_test.hpp>

#include "prompt.hpp"

// --- push and get basics ---

BOOST_AUTO_TEST_CASE(get_returns_nullptr_for_unknown_label) {
  PromptHistory h;
  BOOST_CHECK(h.get("Go to line: ") == nullptr);
}

BOOST_AUTO_TEST_CASE(push_then_get_returns_entries) {
  PromptHistory h;
  h.push("Find: ", "hello");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 1u);
  BOOST_CHECK_EQUAL((*vec)[0], "hello");
}

BOOST_AUTO_TEST_CASE(push_orders_most_recent_first) {
  PromptHistory h;
  h.push("Find: ", "alpha");
  h.push("Find: ", "beta");
  h.push("Find: ", "gamma");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 3u);
  BOOST_CHECK_EQUAL((*vec)[0], "gamma");
  BOOST_CHECK_EQUAL((*vec)[1], "beta");
  BOOST_CHECK_EQUAL((*vec)[2], "alpha");
}

// --- deduplication ---

BOOST_AUTO_TEST_CASE(push_deduplicates_and_moves_to_front) {
  PromptHistory h;
  h.push("Find: ", "alpha");
  h.push("Find: ", "beta");
  h.push("Find: ", "alpha"); // re-push
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 2u);
  BOOST_CHECK_EQUAL((*vec)[0], "alpha");
  BOOST_CHECK_EQUAL((*vec)[1], "beta");
}

// --- empty string rejection ---

BOOST_AUTO_TEST_CASE(push_ignores_empty_strings) {
  PromptHistory h;
  h.push("Find: ", "");
  BOOST_CHECK(h.get("Find: ") == nullptr);
}

BOOST_AUTO_TEST_CASE(push_empty_does_not_affect_existing) {
  PromptHistory h;
  h.push("Find: ", "hello");
  h.push("Find: ", "");
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_REQUIRE_EQUAL(vec->size(), 1u);
  BOOST_CHECK_EQUAL((*vec)[0], "hello");
}

// --- size cap ---

BOOST_AUTO_TEST_CASE(push_caps_at_max_per_label) {
  PromptHistory h;
  for (std::size_t i = 0; i < PromptHistory::kMaxPerLabel + 10; ++i)
    h.push("Find: ", "entry_" + std::to_string(i));
  auto* vec = h.get("Find: ");
  BOOST_REQUIRE(vec != nullptr);
  BOOST_CHECK_EQUAL(vec->size(), PromptHistory::kMaxPerLabel);
  // Most recent is first.
  BOOST_CHECK_EQUAL((*vec)[0], "entry_" + std::to_string(PromptHistory::kMaxPerLabel + 9));
  // Oldest surviving entry.
  BOOST_CHECK_EQUAL((*vec)[PromptHistory::kMaxPerLabel - 1], "entry_10");
}

// --- label isolation ---

BOOST_AUTO_TEST_CASE(separate_labels_are_independent) {
  PromptHistory h;
  h.push("Find: ", "search_term");
  h.push("Go to line: ", "42");
  auto* findVec = h.get("Find: ");
  auto* gotoVec = h.get("Go to line: ");
  BOOST_REQUIRE(findVec != nullptr);
  BOOST_REQUIRE(gotoVec != nullptr);
  BOOST_CHECK_EQUAL(findVec->size(), 1u);
  BOOST_CHECK_EQUAL(gotoVec->size(), 1u);
  BOOST_CHECK_EQUAL((*findVec)[0], "search_term");
  BOOST_CHECK_EQUAL((*gotoVec)[0], "42");
}

// --- buildDimHint ---
//
// Used by the Find / Regex Find prompts to echo the previous search as
// a dim-styled hint when the user re-opens the prompt with empty input.
// The hint text comes from a string previously typed into a prompt, so
// it must be escaped before being painted into the modeline; otherwise
// a control byte that ever slipped past the input filter (or that
// future code paths might admit) would be written verbatim to the
// terminal.

BOOST_AUTO_TEST_CASE(buildDimHint_empty_returns_blank) {
  auto d = buildDimHint("");
  BOOST_CHECK_EQUAL(d.rendered, "");
  BOOST_CHECK_EQUAL(d.visualWidth, 0u);
}

BOOST_AUTO_TEST_CASE(buildDimHint_plain_text_wrapped_in_dim) {
  auto d = buildDimHint("hello");
  BOOST_CHECK_EQUAL(d.rendered, std::string(ansi::Dim) + "hello" + std::string(ansi::DimOff));
  BOOST_CHECK_EQUAL(d.visualWidth, 5u);
}

BOOST_AUTO_TEST_CASE(buildDimHint_escapes_raw_esc) {
  // The regression: a raw ESC in the hint must not reach the terminal
  // intact. escapeNonPrintable replaces it with a \x1b literal wrapped
  // in CtrlColor styling.
  auto d = buildDimHint(std::string("a\x1b[31mb", 7));
  // The rendered string must NOT contain a raw ESC followed by '['
  // (which is the lead-in of a CSI sequence the terminal would
  // interpret).
  bool foundRawCsi = false;
  for (std::size_t i = 0; i + 1 < d.rendered.size(); ++i) {
    auto u = static_cast<unsigned char>(d.rendered[i]);
    // SGR/Dim/DimOff sequences emitted by the framing or by
    // escapeNonPrintable are allowed (terminated by 'm'); only a raw
    // ESC followed by '[' that is then followed by user-controlled
    // bytes is dangerous. Conservative check: no ESC followed by '['
    // followed by a non-control byte other than the SGR digits/`m`.
    if (u == 0x1b && d.rendered[i + 1] == '[') {
      // This is some ANSI sequence. Walk forward to its terminator.
      // We require that what we see between ESC[ and the terminator is
      // only digits, ';' and a 0x40..0x7E terminator — i.e. that it's
      // a well-formed SGR sequence the editor itself emitted.
      std::size_t j = i + 2;
      while (j < d.rendered.size() &&
             ((d.rendered[j] >= '0' && d.rendered[j] <= '9') || d.rendered[j] == ';'))
        ++j;
      if (j >= d.rendered.size() || static_cast<unsigned char>(d.rendered[j]) < 0x40 ||
          static_cast<unsigned char>(d.rendered[j]) > 0x7E) {
        foundRawCsi = true;
        break;
      }
      i = j; // skip past this safe SGR sequence
    }
  }
  BOOST_CHECK(!foundRawCsi);
  // The escaped form should contain the literal characters \x1b
  // (backslash, x, 1, b) rendered for the user to see.
  BOOST_CHECK(d.rendered.find("\\x1b") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(buildDimHint_escapes_bel) {
  // BEL (0x07) is the OSC terminator. Treat the same way.
  auto d = buildDimHint(std::string("\x07", 1));
  // No raw BEL in the output.
  BOOST_CHECK(d.rendered.find('\x07') == std::string::npos);
  BOOST_CHECK(d.rendered.find("\\x07") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(buildDimHint_visualWidth_matches_escaped) {
  // For an all-printable hint the width is just the string length;
  // for a hint with escaped bytes the width reflects the displayed
  // "\xHH" expansion (4 cols per escaped byte).
  auto d1 = buildDimHint("abc");
  BOOST_CHECK_EQUAL(d1.visualWidth, 3u);
  auto d2 = buildDimHint(std::string("\x01", 1));
  // "\x01" displays as 4 visible columns.
  BOOST_CHECK_EQUAL(d2.visualWidth, 4u);
}
