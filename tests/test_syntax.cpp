#define BOOST_TEST_MODULE Syntax
#include <boost/test/unit_test.hpp>

#include "buffer/modes/c/syntax.hpp"
#include "buffer/modes/dif/syntax.hpp"
#include "buffer/modes/jsn/syntax.hpp"
#include "buffer/modes/py/syntax.hpp"
#include "buffer/modes/sh/syntax.hpp"
#include "buffer/modes/tml/syntax.hpp"
#include "buffer/modes/yml/syntax.hpp"
#include "buffer/text_buffer.hpp"
#include "test_helpers.hpp"

// Helper: create a line from a string literal.
static std::string mkline(std::string_view s) { return std::string(s); }

// Helper: check that all bytes in [from, to) have a given token.
static bool allTokens(std::vector<SyntaxToken> const& toks, std::size_t from, std::size_t to,
                      SyntaxToken expected) {
  for (std::size_t i = from; i < to && i < toks.size(); ++i) {
    if (toks[i] != expected)
      return false;
  }
  return from < to;
}

// ============================================================
// Item 1: String highlighting ("...", '...', raw strings)
// ============================================================

BOOST_AUTO_TEST_CASE(double_quoted_string_is_string_token) {
  auto line = mkline(R"(x = "hello";)");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // "hello" is at positions 4..10 (including quotes).
  BOOST_CHECK(allTokens(toks, 4, 11, SyntaxToken::String));
  // x should be Normal.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[0]), static_cast<int>(SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(char_literal_is_string_token) {
  auto line = mkline("char c = 'x';");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 'x' is at positions 9..11.
  BOOST_CHECK(allTokens(toks, 9, 12, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(escaped_quote_in_string) {
  auto line = mkline(R"(s = "a\"b";)");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // The whole "a\"b" should be String.
  BOOST_CHECK(allTokens(toks, 4, 10, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(thousand_separator_not_string) {
  auto line = mkline("int x = 1'000'000;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 1'000'000 is one Number token spanning [8, 17); the ' digit
  // separators (offsets 9 and 13) are part of the number, not String
  // (i.e. not misparsed as character literals).
  BOOST_CHECK(allTokens(toks, 8, 17, SyntaxToken::Number));
  BOOST_CHECK(toks[9] != SyntaxToken::String);
  BOOST_CHECK(toks[13] != SyntaxToken::String);
  // The trailing ';' is not part of the number.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[17]), static_cast<int>(SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(raw_string_single_line) {
  auto line = mkline(R"__(s = R"(hello)";)__");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // R"(hello)" spans from position 4 to 13 inclusive.
  BOOST_CHECK(allTokens(toks, 4, 14, SyntaxToken::String));
  // ; at position 14 is NOT string.
  BOOST_CHECK(toks[14] != SyntaxToken::String);
  BOOST_CHECK(!st.inBlockComment);
  BOOST_CHECK(!st.rawDelim.has_value());
}

BOOST_AUTO_TEST_CASE(raw_string_with_delimiter) {
  auto line = mkline(R"__(s = R"foo(hello)foo";)__");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // R"foo(hello)foo" is all String.
  BOOST_CHECK(allTokens(toks, 4, 20, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value());
}

// A raw-string delimiter is at most 16 characters without spaces,
// parentheses, backslashes or quotes; anything else after R" is an
// ordinary string. Searching the whole rest of the line for a '(' made a
// line of R"R"R"... quadratic.
BOOST_AUTO_TEST_CASE(raw_string_needs_a_valid_delimiter) {
  auto line = mkline(R"__(a = R"x y"; f(1);)__");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 5, 10, SyntaxToken::String)); // "x y"
  BOOST_CHECK(toks[12] != SyntaxToken::String);             // f
  BOOST_CHECK(!st.rawDelim.has_value());

  auto tooLong = mkline(R"__(R"abcdefghijklmnopq(x)abcdefghijklmnopq")__");
  CSyntaxState st2;
  tokenizeLine(tooLong, st2, toks);
  BOOST_CHECK(!st2.rawDelim.has_value());
  CSyntaxState st3;
  advanceBlockCommentState(tooLong, st3);
  BOOST_CHECK(!st3.rawDelim.has_value());

  auto longest = mkline(R"__(R"abcdefghijklmnop(x)__");
  CSyntaxState st4;
  tokenizeLine(longest, st4, toks);
  BOOST_REQUIRE(st4.rawDelim.has_value());
  BOOST_CHECK_EQUAL(st4.rawDelim->view(), "abcdefghijklmnop");
}

BOOST_AUTO_TEST_CASE(raw_string_prefix_run_is_linear) {
  std::string content;
  for (int i = 0; i < 200000; ++i)
    content += "R\"";
  auto line = mkline(content);
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks); // took ~10 s when quadratic
  BOOST_CHECK_EQUAL(toks.size(), line.size());
  CSyntaxState st2;
  advanceBlockCommentState(line, st2);
  BOOST_CHECK(!st2.rawDelim.has_value());
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_start) {
  auto line = mkline(R"__(s = R"(hello)__");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // Everything from R onwards is String.
  BOOST_CHECK(allTokens(toks, 4, line.size(), SyntaxToken::String));
  // Raw string continues to next line.
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(st.rawDelim->view(), "");
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_end) {
  // Simulate second line of a multiline raw string with empty delimiter.
  // Line content: world)";
  std::string content = "world)\";";
  auto line = mkline(content);
  CSyntaxState st;
  st.rawDelim.emplace(""); // continuing from previous line
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // Everything up to and including )" should be String (positions 0..6).
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value()); // raw string ended
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_with_delim) {
  auto line1 = mkline(R"__(R"foo(start)__");
  CSyntaxState st;
  std::vector<SyntaxToken> toks1;
  tokenizeLine(line1, st, toks1);
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(st.rawDelim->view(), "foo");

  auto line2 = mkline(R"__(middle)__");
  std::vector<SyntaxToken> toks2;
  tokenizeLine(line2, st, toks2);
  BOOST_CHECK(allTokens(toks2, 0, line2.size(), SyntaxToken::String));
  BOOST_CHECK(st.rawDelim.has_value()); // still inside

  // end)foo";  — the )foo" closes the raw string.
  auto line3 = mkline(R"__(end)foo";)__");
  std::vector<SyntaxToken> toks3;
  tokenizeLine(line3, st, toks3);
  // end)foo" = 8 chars of String (positions 0-7), ; at 8 is not.
  BOOST_CHECK(allTokens(toks3, 0, 8, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value()); // ended
}

BOOST_AUTO_TEST_CASE(string_render_uses_red_color) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, R"(x = "hello";)");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // String color (red): \x1b[38;2;200;80;80m
  BOOST_CHECK(out.find("\x1b[38;2;200;80;80m") != std::string::npos);
}

// ============================================================
// Item 2: Angle bracket operator heuristic
// ============================================================

BOOST_AUTO_TEST_CASE(shift_left_is_operator) {
  auto line = mkline("x = a << b;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // << at positions 6,7 should be Operator.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(shift_right_is_operator) {
  auto line = mkline("x = a >> b;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(less_equal_is_operator) {
  auto line = mkline("if (a <= b)");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // <= at positions 6,7.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(greater_equal_is_operator) {
  auto line = mkline("if (a >= b)");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(spaced_less_than_is_operator) {
  auto line = mkline("if (a < b)");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // < at position 6 with spaces around it → Operator.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(template_angle_is_angle) {
  auto line = mkline("vector<int> v;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // < at position 6 — no spaces → Angle.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Angle));
  // > at position 10 — no spaces → Angle.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Angle));
}

BOOST_AUTO_TEST_CASE(spaceship_is_operator) {
  auto line = mkline("a <=> b");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // <=> at positions 2,3,4.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[4]), static_cast<int>(SyntaxToken::Operator));
}

// ============================================================
// Item 3: '?' bracket jump (gotoMatchingBracket)
// ============================================================

BOOST_AUTO_TEST_CASE(goto_matching_paren_forward) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar)");
  buf.setPoint(0, 3); // on '('
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 7u); // on ')'
}

BOOST_AUTO_TEST_CASE(goto_matching_paren_backward) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar)");
  buf.setPoint(0, 7); // on ')'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 3u); // on '('
}

BOOST_AUTO_TEST_CASE(goto_matching_brace) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "{ x }");
  buf.setPoint(0, 0); // on '{'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 4u); // on '}'
}

BOOST_AUTO_TEST_CASE(goto_matching_bracket_square) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "a[b]");
  buf.setPoint(0, 1); // on '['
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 3u); // on ']'
}

BOOST_AUTO_TEST_CASE(goto_matching_angle) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "vector<int>");
  buf.setPoint(0, 6); // on '<'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 10u); // on '>'
}

BOOST_AUTO_TEST_CASE(goto_matching_multiline) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  // Auto-indent in Cpp mode copies leading whitespace on newline.
  // "if (\n  x\n)" produces: line 0 = "if (", line 1 = "  x",
  // line 2 = "  )" (the ')' gets 2-space indent from line 1).
  insertString(buf, "if (\n  x\n)");
  buf.setPoint(0, 3); // on '(' at line 0
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().line, 2u);
  BOOST_CHECK_EQUAL(buf.point().offset, 2u); // ')' is after auto-indent
}

BOOST_AUTO_TEST_CASE(goto_matching_not_on_bracket) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "hello");
  buf.setPoint(0, 0); // on 'h'
  BOOST_CHECK(!buf.gotoMatchingBracket());
}

BOOST_AUTO_TEST_CASE(goto_matching_no_match) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar");
  buf.setPoint(0, 3); // on '(' with no matching ')'
  BOOST_CHECK(!buf.gotoMatchingBracket());
}

// Mode-agnostic fallback: parens/brackets/braces match in non-Cpp/Jsn
// modes too (the fallback uses the cpp matcher with all-Normal tokens,
// which only matches `<>` when tagged Angle so plain text comparison
// operators stay inert).

BOOST_AUTO_TEST_CASE(goto_matching_in_txt_mode_paren) {
  TB<TextBuffer> buf("notes.txt");
  // Default mode is Txt. Confirm and exercise the fallback.
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
  insertString(buf, "abc(def)ghi");
  buf.setPoint(0, 3); // on '('
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 7u);
}

BOOST_AUTO_TEST_CASE(goto_matching_in_txt_mode_brace) {
  TB<TextBuffer> buf("notes.txt");
  insertString(buf, "{ key = value }");
  buf.setPoint(0, 0);
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 14u);
}

BOOST_AUTO_TEST_CASE(goto_matching_in_md_mode_brackets) {
  TB<TextBuffer> buf("readme.md");
  buf.setMode(BufferMode::Md);
  insertString(buf, "see [link] there");
  buf.setPoint(0, 4); // on '['
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 9u);
}

BOOST_AUTO_TEST_CASE(goto_matching_in_txt_does_not_match_lt_gt) {
  // Plain '<' in txt should not be treated as a bracket — the cpp
  // matcher only honours <> when tokens are tagged Angle.
  TB<TextBuffer> buf("notes.txt");
  insertString(buf, "if a < b then");
  buf.setPoint(0, 5); // on '<'
  BOOST_CHECK(!buf.gotoMatchingBracket());
}

// ============================================================
// Item 4: Brackets have no color but match in turquoise
// ============================================================

BOOST_AUTO_TEST_CASE(brackets_not_colored_individually) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 0); // cursor NOT on bracket
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Old bracket colors should not appear.
  // Old paren red: \x1b[38;2;200;80;80m — now used for String, not brackets.
  // Old bracket orange: \x1b[38;2;180;140;40m
  // Old brace purple: \x1b[38;2;150;100;200m — now used for Operator.
  // Without any strings or operators, none of these should appear.
  BOOST_CHECK(out.find("\x1b[38;2;180;140;40m") == std::string::npos); // no orange
}

BOOST_AUTO_TEST_CASE(bracket_match_uses_turquoise) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 3); // on '(' — should match ')'
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Turquoise: \x1b[38;2;60;170;170m
  BOOST_CHECK(out.find("\x1b[38;2;60;170;170m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(bracket_at_point_also_highlighted) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 3); // on '('
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Both the bracket at point and the matching one should be turquoise.
  // Count occurrences of the turquoise color.
  std::string turq = "\x1b[38;2;60;170;170m";
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = out.find(turq, pos)) != std::string::npos) {
    ++count;
    pos += turq.size();
  }
  // Should appear at least twice (once for '(' and once for ')').
  BOOST_CHECK_GE(count, 2u);
}

// ============================================================
// Item 5: true/false/nullptr are literal values, coloured like
// numeric literals (Number), not as keywords. if/for/etc. stay
// keywords. Consistent with JSON/YAML/Python literal colouring.
// ============================================================

BOOST_AUTO_TEST_CASE(true_is_value_literal) {
  auto line = mkline("bool x = true;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // "true" occupies positions 9..12.
  BOOST_CHECK(allTokens(toks, 9, 13, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(false_is_value_literal) {
  auto line = mkline("bool x = false;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // "false" occupies positions 9..13.
  BOOST_CHECK(allTokens(toks, 9, 14, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(nullptr_is_value_literal) {
  auto line = mkline("return nullptr;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // "return" at 0..5 stays Keyword.
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::Keyword));
  // "nullptr" at 7..13 is a literal value (Number colour).
  BOOST_CHECK(allTokens(toks, 7, 14, SyntaxToken::Number));
}

// ============================================================
// Item 5b: Number highlighting (C/C++)
// ============================================================

BOOST_AUTO_TEST_CASE(cpp_integer_is_number) {
  auto line = mkline("int x = 42;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 8, 10, SyntaxToken::Number));                             // 42
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Normal)); // ;
}

BOOST_AUTO_TEST_CASE(cpp_float_with_exponent_is_number) {
  auto line = mkline("double r = 3.14e-2;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 3.14e-2 spans [11, 18).
  BOOST_CHECK(allTokens(toks, 11, 18, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(cpp_hex_literal_is_number) {
  auto line = mkline("auto m = 0xFF00u;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 0xFF00u spans [9, 16) — prefix, hex digits and the u suffix.
  BOOST_CHECK(allTokens(toks, 9, 16, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(cpp_binary_and_suffix_is_number) {
  auto line = mkline("auto b = 0b1010'1100ULL;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 0b1010'1100ULL spans [9, 23).
  BOOST_CHECK(allTokens(toks, 9, 23, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(cpp_leading_dot_float_is_number) {
  auto line = mkline("float f = .5f;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // .5f spans [10, 13).
  BOOST_CHECK(allTokens(toks, 10, 13, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(cpp_hex_float_is_number) {
  auto line = mkline("double h = 0x1.8p3;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // 0x1.8p3 spans [11, 18).
  BOOST_CHECK(allTokens(toks, 11, 18, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(cpp_identifier_with_digits_is_not_number) {
  auto line = mkline("int x1 = y2;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // x1 and y2 are identifiers, not numbers.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[4]), static_cast<int>(SyntaxToken::Normal));  // x
  BOOST_CHECK_EQUAL(static_cast<int>(toks[5]), static_cast<int>(SyntaxToken::Normal));  // 1
  BOOST_CHECK_EQUAL(static_cast<int>(toks[9]), static_cast<int>(SyntaxToken::Normal));  // y
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Normal)); // 2
}

BOOST_AUTO_TEST_CASE(cpp_number_does_not_swallow_following_operator) {
  auto line = mkline("n = 3+4;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[4]), static_cast<int>(SyntaxToken::Number));   // 3
  BOOST_CHECK_EQUAL(static_cast<int>(toks[5]), static_cast<int>(SyntaxToken::Operator)); // +
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Number));   // 4
}

BOOST_AUTO_TEST_CASE(cpp_number_render_uses_turquoise_color) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "int n = 42;");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Number color (light turquoise): \x1b[38;2;100;200;200m
  BOOST_CHECK(out.find("\x1b[38;2;100;200;200m") != std::string::npos);
}

// ============================================================
// Item 6: Operator highlighting
// ============================================================

BOOST_AUTO_TEST_CASE(equals_is_operator) {
  auto line = mkline("x = 5;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(not_equals_is_operator) {
  auto line = mkline("x != y");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // != at positions 2,3.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(logical_and_is_operator) {
  auto line = mkline("a && b");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(logical_or_is_operator) {
  auto line = mkline("a || b");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(ternary_is_operator) {
  auto line = mkline("x ? a : b");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(plus_minus_star_are_operators) {
  auto line = mkline("a + b - c * d");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(tilde_is_operator) {
  auto line = mkline("~x");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[0]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(operator_render_uses_purple_color) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "x = 5;");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Operator color (purple): \x1b[38;2;150;100;200m
  BOOST_CHECK(out.find("\x1b[38;2;150;100;200m") != std::string::npos);
}

// ============================================================
// Preprocessor directive highlighting
// ============================================================

BOOST_AUTO_TEST_CASE(preprocessor_include) {
  auto line = mkline("#include <stdio.h>");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Preprocessor));
  BOOST_CHECK(!st.inPreprocessor);
}

BOOST_AUTO_TEST_CASE(preprocessor_define) {
  auto line = mkline("#define FOO 42");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Preprocessor));
}

BOOST_AUTO_TEST_CASE(preprocessor_with_leading_whitespace) {
  auto line = mkline("  #ifdef BAR");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Preprocessor));
}

BOOST_AUTO_TEST_CASE(preprocessor_continuation_backslash) {
  auto line1 = mkline("#define MULTI \\");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line1, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line1.size(), SyntaxToken::Preprocessor));
  BOOST_CHECK(st.inPreprocessor);

  auto line2 = mkline("  line2 \\");
  tokenizeLine(line2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line2.size(), SyntaxToken::Preprocessor));
  BOOST_CHECK(st.inPreprocessor);

  auto line3 = mkline("  line3");
  tokenizeLine(line3, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line3.size(), SyntaxToken::Preprocessor));
  BOOST_CHECK(!st.inPreprocessor);
}

BOOST_AUTO_TEST_CASE(preprocessor_not_triggered_by_normal_code) {
  auto line = mkline("int x = 5;");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(toks[0] != SyntaxToken::Preprocessor);
}

BOOST_AUTO_TEST_CASE(preprocessor_not_in_block_comment) {
  auto line = mkline("#define FOO");
  CSyntaxState st;
  st.inBlockComment = true;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Comment));
}

BOOST_AUTO_TEST_CASE(preprocessor_advance_state_tracks_continuation) {
  auto line1 = mkline("#define X \\");
  CSyntaxState st;
  advanceBlockCommentState(line1, st);
  BOOST_CHECK(st.inPreprocessor);

  auto line2 = mkline("  Y");
  advanceBlockCommentState(line2, st);
  BOOST_CHECK(!st.inPreprocessor);
}

BOOST_AUTO_TEST_CASE(preprocessor_render_uses_amber_color) {
  TB<TextBuffer> buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "#include <x>");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  BOOST_CHECK(out.find("\x1b[38;2;180;140;60m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(preprocessor_bracket_not_matched) {
  auto lines = toLineStore({"#include <stdio.h>", "foo(<bar>);"});
  auto result = findMatchingBracket(lines, {0, 9});
  BOOST_CHECK(!result.has_value());
}

// ============================================================
// advanceBlockCommentState with raw strings
// ============================================================

BOOST_AUTO_TEST_CASE(advance_state_skips_raw_string) {
  auto line1 = mkline(R"__(R"(hello)__");
  CSyntaxState st;
  advanceBlockCommentState(line1, st);
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(st.rawDelim->view(), "");
  BOOST_CHECK(!st.inBlockComment);

  std::string content2 = "world)\";";
  auto line2 = mkline(content2);
  advanceBlockCommentState(line2, st);
  BOOST_CHECK(!st.rawDelim.has_value());
  BOOST_CHECK(!st.inBlockComment);
}

BOOST_AUTO_TEST_CASE(advance_state_block_comment_inside_raw_string_ignored) {
  // A /* inside a raw string should not start a block comment.
  auto line = mkline(R"__(R"(/* not a comment */)")__");
  CSyntaxState st;
  advanceBlockCommentState(line, st);
  BOOST_CHECK(!st.inBlockComment);
  BOOST_CHECK(!st.rawDelim.has_value());
}

// ============================================================
// Bug 1: findMatchingBracket must skip brackets in strings/comments
// ============================================================

// Build lines from newline-separated text. The bracket matchers take
// `LineStore const&`, so tests build a store directly instead of strings.
static LineStore mklines(std::string_view s) {
  LineStore result;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      result.push_back(makeLine(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  return result;
}

BOOST_AUTO_TEST_CASE(bracket_in_string_skipped_finds_real_close) {
  // printf("(");  — cursor on outer '(' at offset 6.
  // The '(' inside the string at offset 8 must be ignored.
  // The matching ')' is the one at offset 10, outside the string.
  auto lines = mklines("printf(\"(\");");
  auto result = findMatchingBracket(lines, {0, 6});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 10u); // ')' after closing '"'
}

BOOST_AUTO_TEST_CASE(bracket_in_string_no_outer_match) {
  // A '(' that only has a matching ')' inside a string — no real match.
  // open( "string with ) inside" and nothing after.
  auto lines = mklines("open(\")\");");
  // '(' at offset 4; ')' at offset 6 is inside string; ')' at offset 8 is the real close.
  auto result = findMatchingBracket(lines, {0, 4});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 8u);
}

BOOST_AUTO_TEST_CASE(bracket_in_block_comment_not_matched) {
  // if (x /* ) */ && y) {} — the ')' inside /* */ is not a real bracket.
  auto lines = mklines("if (x /* ) */ && y) {}");
  // '(' at offset 3; ')' at offset 9 is inside block comment; ')' at offset 18 is real.
  auto result = findMatchingBracket(lines, {0, 3});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 18u);
}

BOOST_AUTO_TEST_CASE(bracket_in_line_comment_not_matched) {
  // foo(x); // (unmatched) — the '(' in the comment should be ignored.
  auto lines = mklines("foo(x); // (unmatched)");
  // '(' at offset 3 matches ')' at offset 5; line comment '(' at 11 is irrelevant.
  auto result = findMatchingBracket(lines, {0, 3});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 5u);
}

BOOST_AUTO_TEST_CASE(open_bracket_inside_string_returns_nullopt) {
  // Cursor sits on '(' that is itself inside a string literal — not a bracket.
  auto lines = mklines("x = \"(\";");
  // '(' at offset 5 is inside string.
  auto result = findMatchingBracket(lines, {0, 5});
  BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(close_bracket_in_string_not_counted_backward) {
  // Backward scan: ')' inside string should not be counted.
  // foo(")", x) — cursor on ')' at offset 11.
  auto lines = mklines("foo(\")\", x)");
  auto result = findMatchingBracket(lines, {0, 10});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 3u);
}

// ============================================================
// Bug 2: angle brackets use Angle/Operator distinction
// ============================================================

BOOST_AUTO_TEST_CASE(angle_operator_not_matched) {
  // x < y && z > w — cursor on '<' which is Operator, should not match.
  auto lines = mklines("x < y && z > w");
  // '<' at offset 2 is Operator (spaces around it).
  auto result = findMatchingBracket(lines, {0, 2});
  BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(angle_template_matched) {
  // vector<int> — cursor on '<' which is Angle.
  auto lines = mklines("vector<int>");
  auto result = findMatchingBracket(lines, {0, 6});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 10u);
}

BOOST_AUTO_TEST_CASE(angle_close_operator_not_matched_backward) {
  // '>' as Operator: backward scan from '>' in 'x > y' should return nullopt.
  auto lines = mklines("x > y");
  // '>' at offset 2 — spaces around it → Operator.
  auto result = findMatchingBracket(lines, {0, 2});
  BOOST_CHECK(!result.has_value());
}

BOOST_AUTO_TEST_CASE(angle_template_backward_matched) {
  // vector<int> — cursor on '>'.
  auto lines = mklines("vector<int>");
  auto result = findMatchingBracket(lines, {0, 10});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 6u);
}

BOOST_AUTO_TEST_CASE(nested_angle_templates_matched) {
  // pair<char, int> — cursor on outer '<' at offset 4.
  // Uses a single closing '>' (not '>>') to avoid the >> tokenizer heuristic.
  auto lines = mklines("pair<char, int>");
  auto result = findMatchingBracket(lines, {0, 4});
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 14u);
}

// ============================================================
// findMatchingBracket overload with external token cache
// ============================================================

BOOST_AUTO_TEST_CASE(findMatchingBracket_with_token_cache_consulted) {
  // The three-argument overload must query the caller-supplied token
  // source for every visited line and must not fall back to an
  // internal tokenization. We verify this by handing in a provider
  // that returns tokens coloring '(' at offset 10 as Comment — which
  // isBracketChar rejects, so the forward scan should skip it and
  // match the next real ')' at offset 13.
  auto lines = mklines("a(x) + y( )  )");
  //                    0123456789012345
  // Normal tokenization would match '(' at offset 8 to ')' at 10.
  // With our custom provider we mark the real ')' at 10 as Comment,
  // so it is ignored and depth drops to 0 at ')' at offset 13.
  std::vector<SyntaxToken> toks(lines[0]->size(), SyntaxToken::Normal);
  toks[1] = SyntaxToken::Paren;
  toks[3] = SyntaxToken::Paren;
  toks[8] = SyntaxToken::Paren;
  toks[10] = SyntaxToken::Comment; // hide the nearer ')'
  toks[13] = SyntaxToken::Paren;
  int callCount = 0;
  auto getTokens = [&](std::size_t i) -> std::span<SyntaxToken const> {
    ++callCount;
    BOOST_REQUIRE_EQUAL(i, 0u);
    return toks;
  };
  auto result = findMatchingBracket(lines, {0, 8}, getTokens);
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 13u);
  // Provider must have been consulted at least once.
  BOOST_CHECK_GT(callCount, 0);
}

BOOST_AUTO_TEST_CASE(findMatchingBracket_with_token_cache_backward) {
  // Backward scan: hide a nearer ')' so depth does not increment at
  // that position, causing the outermost cursor to match a closer
  // '(' than it normally would.
  auto lines = mklines("( ( ) ( ) )");
  //                    0         1
  //                    01234567890
  // Normal tokenization matches offset 10 to offset 0. Hiding the
  // ')' at offset 8 drops one increment, so depth hits 0 at offset 6.
  std::vector<SyntaxToken> toks(lines[0]->size(), SyntaxToken::Normal);
  for (std::size_t o : {0u, 2u, 4u, 6u, 10u})
    toks[o] = SyntaxToken::Paren;
  toks[8] = SyntaxToken::String; // hide this ')'
  auto getTokens = [&](std::size_t) -> std::span<SyntaxToken const> { return toks; };
  auto result = findMatchingBracket(lines, {0, 10}, getTokens);
  BOOST_REQUIRE(result.has_value());
  BOOST_CHECK_EQUAL(result->offset, 6u);
}

// ============================================================
// `for` followed by a UTF-8 lead byte (e.g. an identifier `forπ`) used to
// be tokenized as a keyword `for` plus stray bytes, because `isIdentChar`
// rejected high-bit bytes. Treating any byte >= 0x80 as identifier-class
// keeps the keyword from leaking out of a non-ASCII identifier.
BOOST_AUTO_TEST_CASE(non_ascii_byte_does_not_split_identifier) {
  auto line = mkline("forπ"); // π is two UTF-8 bytes (0xCF 0x80)
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // `for` should NOT be highlighted as a keyword inside this identifier.
  for (std::size_t i = 0; i < 3; ++i)
    BOOST_CHECK(toks[i] != SyntaxToken::Keyword);
}

// Operator merge previously had no upper bound and would coalesce any run
// of operator-class characters into one logical operator span. Real C++
// operators top out at 3 chars (<<=, >>=, ->*); cap the merge there.
BOOST_AUTO_TEST_CASE(operator_run_caps_at_three_characters) {
  auto line = mkline("a +++++++ b");
  CSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeLine(line, st, toks);
  // All seven '+' chars stay Operator-typed (visually the same), but the
  // tokenization no longer treats them as one merged token. We only
  // assert the styling invariant here.
  for (std::size_t i = 2; i < 9; ++i)
    BOOST_CHECK(toks[i] == SyntaxToken::Operator);
}

// ============================================================
// Markdown tokenizer
// ============================================================

#include "buffer/modes/md/syntax.hpp"

BOOST_AUTO_TEST_CASE(md_heading_single_hash) {
  auto line = mkline("# Hello");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Heading));
}

BOOST_AUTO_TEST_CASE(md_heading_multiple_hashes) {
  auto line = mkline("### Sub-heading");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Heading));
}

BOOST_AUTO_TEST_CASE(md_heading_hash_only) {
  // A line with just "#" (no space after) should still be a heading.
  auto line = mkline("#");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 1, SyntaxToken::Heading));
}

BOOST_AUTO_TEST_CASE(md_not_heading_no_space) {
  // "#foo" — no space after '#' → not a heading.
  auto line = mkline("#foo");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_bold) {
  auto line = mkline("some **bold** text");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  // "**bold**" is at positions 5..12 (8 bytes).
  BOOST_CHECK(allTokens(toks, 5, 13, SyntaxToken::Bold));
  // Surrounding text is Normal.
  BOOST_CHECK(allTokens(toks, 0, 5, SyntaxToken::Normal));
  BOOST_CHECK(allTokens(toks, 13, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_bold_unclosed) {
  // Unclosed bold: **not closed — should remain Normal.
  auto line = mkline("**not closed");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_link) {
  auto line = mkline("see [click](http://example.com) here");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  // "[click](http://example.com)" is at positions 4..30 (27 bytes).
  BOOST_CHECK(allTokens(toks, 4, 31, SyntaxToken::Link));
  // Surrounding text is Normal.
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Normal));
  BOOST_CHECK(allTokens(toks, 31, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_link_no_paren) {
  // "[text]" without "(url)" — not a link.
  auto line = mkline("just [text] here");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_code_span_suppresses_link) {
  // An unbalanced '[' inside a backtick code span must not open a link,
  // and must not carry MdLink state onto the next line. Without the
  // code-span skip, `arr[i` set MdLink::Label and painted "[i" as Link.
  auto line = mkline("`arr[i`");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(st.mdLink == MdLink::None);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(md_code_span_then_link_still_highlights) {
  // A code span containing a stray '[' is skipped, but a real link
  // after it must still highlight.
  auto line = mkline("`a[b` [x](y)");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(st.mdLink == MdLink::None);
  // "[x](y)" occupies positions 6..11.
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::Normal));
  BOOST_CHECK(allTokens(toks, 6, 12, SyntaxToken::Link));
}

BOOST_AUTO_TEST_CASE(md_empty_line) {
  auto line = mkline("");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(toks.empty());
}

BOOST_AUTO_TEST_CASE(md_bold_and_link_same_line) {
  auto line = mkline("**bold** and [link](url)");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 8, SyntaxToken::Bold));
  BOOST_CHECK(allTokens(toks, 13, 24, SyntaxToken::Link));
}

// Unclosed bold previously caused the inner scan to walk to end-of-line and
// the outer loop to break, leaving any subsequent markup unhighlighted.
// Now: the unmatched `**` is left as Normal and scanning resumes past it
// so a trailing link still highlights.
BOOST_AUTO_TEST_CASE(md_unclosed_bold_does_not_eat_trailing_link) {
  auto line = mkline("**unfinished [link](url)");
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(line, st, toks);
  // The `**` itself stays Normal.
  BOOST_CHECK(allTokens(toks, 0, 2, SyntaxToken::Normal));
  // The link "[link](url)" at position 13..24 must highlight.
  BOOST_CHECK(allTokens(toks, 13, 24, SyntaxToken::Link));
}

BOOST_AUTO_TEST_CASE(md_link_label_spans_two_lines) {
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("see [an external");
  tokenizeMarkdownLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Normal));
  BOOST_CHECK(allTokens(toks, 4, l1.size(), SyntaxToken::Link)); // [an external
  BOOST_CHECK(st.mdLink == MdLink::Label);

  auto l2 = mkline("site](http://example.org) end");
  tokenizeMarkdownLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 25, SyntaxToken::Link)); // site](...)
  BOOST_CHECK(allTokens(toks, 25, l2.size(), SyntaxToken::Normal));
  BOOST_CHECK(st.mdLink == MdLink::None);
}

BOOST_AUTO_TEST_CASE(md_link_url_spans_two_lines) {
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("[text](http://");
  tokenizeMarkdownLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l1.size(), SyntaxToken::Link));
  BOOST_CHECK(st.mdLink == MdLink::Url);

  auto l2 = mkline("example.com) rest");
  tokenizeMarkdownLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 12, SyntaxToken::Link)); // example.com)
  BOOST_CHECK(allTokens(toks, 12, l2.size(), SyntaxToken::Normal));
  BOOST_CHECK(st.mdLink == MdLink::None);
}

BOOST_AUTO_TEST_CASE(md_link_label_spans_three_lines) {
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("[abc");
  tokenizeMarkdownLine(l1, st, toks);
  BOOST_CHECK(st.mdLink == MdLink::Label);
  auto l2 = mkline("def");
  tokenizeMarkdownLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::Link));
  BOOST_CHECK(st.mdLink == MdLink::Label);
  auto l3 = mkline("ghi](url) z");
  tokenizeMarkdownLine(l3, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 9, SyntaxToken::Link));
  BOOST_CHECK(allTokens(toks, 9, l3.size(), SyntaxToken::Normal));
  BOOST_CHECK(st.mdLink == MdLink::None);
}

BOOST_AUTO_TEST_CASE(md_unterminated_link_colors_to_end_of_paragraph) {
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("[never closed");
  tokenizeMarkdownLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l1.size(), SyntaxToken::Link));
  auto l2 = mkline("still going");
  tokenizeMarkdownLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::Link));
  BOOST_CHECK(st.mdLink == MdLink::Label); // stays open within the paragraph

  // A blank line (whitespace counts) ends the paragraph and the link.
  auto l3 = mkline("  ");
  tokenizeMarkdownLine(l3, st, toks);
  BOOST_CHECK(st.mdLink == MdLink::None);
  auto l4 = mkline("# a heading again");
  tokenizeMarkdownLine(l4, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l4.size(), SyntaxToken::Heading));
}

BOOST_AUTO_TEST_CASE(md_unmatched_bracket_in_prose_stays_in_its_paragraph) {
  // "[0, 1)" has no ']': every later line used to be link text until some
  // ']' turned up, however far down the file.
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeMarkdownLine(mkline("The interval [0, 1) is half-open."), st, toks);
  BOOST_CHECK(st.mdLink == MdLink::Label);
  tokenizeMarkdownLine(mkline(""), st, toks);
  BOOST_CHECK(st.mdLink == MdLink::None);
  auto next = mkline("An unrelated paragraph.");
  tokenizeMarkdownLine(next, st, toks);
  BOOST_CHECK(allTokens(toks, 0, next.size(), SyntaxToken::Normal));

  // The same for an open destination.
  tokenizeMarkdownLine(mkline("see [text](http://"), st, toks);
  BOOST_CHECK(st.mdLink == MdLink::Url);
  tokenizeMarkdownLine(mkline(""), st, toks);
  BOOST_CHECK(st.mdLink == MdLink::None);
}

BOOST_AUTO_TEST_CASE(md_heading_suppressed_during_link_continuation) {
  MdSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("[link text");
  tokenizeMarkdownLine(l1, st, toks);
  BOOST_CHECK(st.mdLink == MdLink::Label);
  // A '#' here is link-label text, not a heading.
  auto l2 = mkline("# not a heading](url)");
  tokenizeMarkdownLine(l2, st, toks);
  BOOST_CHECK(toks[0] == SyntaxToken::Link);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::Link));
  BOOST_CHECK(st.mdLink == MdLink::None);
}

BOOST_AUTO_TEST_CASE(md_mode_detection) {
  TempFile tmp("# Hello\n", ".md");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Md);
}

BOOST_AUTO_TEST_CASE(md_render_heading_uses_bold_blue) {
  TB<TextBuffer> buf("test.md");
  buf.setMode(BufferMode::Md);
  insertString(buf, "# Title");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Heading color: bold + blue \x1b[1;38;2;80;130;200m
  BOOST_CHECK(out.find("\x1b[1;38;2;80;130;200m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(md_render_bold_uses_bold) {
  TB<TextBuffer> buf("test.md");
  buf.setMode(BufferMode::Md);
  insertString(buf, "some **bold** text");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Bold: \x1b[1m
  BOOST_CHECK(out.find("\x1b[1m") != std::string::npos);
}

// ============================================================
// JSON mode
// ============================================================

BOOST_AUTO_TEST_CASE(json_string_is_string_token) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline(R"("hello")"), toks);
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(json_string_with_escape) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline(R"("a\"b")"), toks);
  // All 6 bytes should be String: " a \ " b "
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(json_true_is_value_literal) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("true"), toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_false_is_value_literal) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("false"), toks);
  BOOST_CHECK(allTokens(toks, 0, 5, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_null_is_value_literal) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("null"), toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_integer_is_number) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("42"), toks);
  BOOST_CHECK(allTokens(toks, 0, 2, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_negative_is_number) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("-3"), toks);
  BOOST_CHECK(allTokens(toks, 0, 2, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_decimal_is_number) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("3.14"), toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_scientific_is_number) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("1.5e10"), toks);
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::Number));
}

// RFC 8259 disallows trailing '.' or empty exponent. The highlighter now
// stops the number token at the integer prefix rather than greedily
// extending across an invalid suffix.
BOOST_AUTO_TEST_CASE(json_trailing_dot_does_not_extend_number_token) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("1."), toks);
  BOOST_CHECK(allTokens(toks, 0, 1, SyntaxToken::Number));
  BOOST_CHECK(allTokens(toks, 1, 2, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(json_empty_exponent_does_not_extend_number_token) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("1e"), toks);
  BOOST_CHECK(allTokens(toks, 0, 1, SyntaxToken::Number));
  BOOST_CHECK(allTokens(toks, 1, 2, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(json_scientific_negative_exp) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("2E-3"), toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Number));
}

BOOST_AUTO_TEST_CASE(json_brackets_are_bracket) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("[1]"), toks);
  BOOST_CHECK(toks[0] == SyntaxToken::Bracket);
  BOOST_CHECK(toks[2] == SyntaxToken::Bracket);
}

BOOST_AUTO_TEST_CASE(json_braces_are_brace) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("{}"), toks);
  BOOST_CHECK(toks[0] == SyntaxToken::Brace);
  BOOST_CHECK(toks[1] == SyntaxToken::Brace);
}

BOOST_AUTO_TEST_CASE(json_no_comments) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline("// foo"), toks);
  // In JSON, // is NOT a comment — should remain Normal.
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(json_colon_comma_normal) {
  std::vector<SyntaxToken> toks;
  tokenizeJsonLine(mkline(": ,"), toks);
  BOOST_CHECK(allTokens(toks, 0, 3, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(json_bracket_match_square) {
  auto lines = toLineStore({"[1, 2]"});
  auto match = findMatchingBracketJson(lines, {0, 0});
  BOOST_REQUIRE(match.has_value());
  BOOST_CHECK_EQUAL(match->line, 0u);
  BOOST_CHECK_EQUAL(match->offset, 5u);
  // Reverse.
  auto rev = findMatchingBracketJson(lines, {0, 5});
  BOOST_REQUIRE(rev.has_value());
  BOOST_CHECK_EQUAL(rev->line, 0u);
  BOOST_CHECK_EQUAL(rev->offset, 0u);
}

BOOST_AUTO_TEST_CASE(json_bracket_match_brace) {
  auto lines = toLineStore({R"({"a": 1})"});
  auto match = findMatchingBracketJson(lines, {0, 0});
  BOOST_REQUIRE(match.has_value());
  BOOST_CHECK_EQUAL(match->offset, 7u);
}

BOOST_AUTO_TEST_CASE(json_bracket_inside_string_skipped) {
  auto lines = toLineStore({R"({"key": "["})"});
  // The [ at pos 8 is inside a string — cursor on { at 0 should match } at 11.
  auto match = findMatchingBracketJson(lines, {0, 0});
  BOOST_REQUIRE(match.has_value());
  BOOST_CHECK_EQUAL(match->offset, 11u);
}

BOOST_AUTO_TEST_CASE(json_bracket_multiline) {
  auto lines = toLineStore({"{", "  \"a\": 1", "}"});
  auto match = findMatchingBracketJson(lines, {0, 0});
  BOOST_REQUIRE(match.has_value());
  BOOST_CHECK_EQUAL(match->line, 2u);
  BOOST_CHECK_EQUAL(match->offset, 0u);
}

BOOST_AUTO_TEST_CASE(json_mode_detection) {
  TempFile tmp("{}\n", ".json");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Jsn);
}

BOOST_AUTO_TEST_CASE(json_render_string_red) {
  TB<TextBuffer> buf("test.json");
  buf.setMode(BufferMode::Jsn);
  insertString(buf, R"("hello")");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // String color: red \x1b[38;2;200;80;80m
  BOOST_CHECK(out.find("\x1b[38;2;200;80;80m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(json_render_value_literal_turquoise) {
  TB<TextBuffer> buf("test.json");
  buf.setMode(BufferMode::Jsn);
  insertString(buf, "true");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // true/false/null share the numeric-literal colour: light
  // turquoise \x1b[38;2;100;200;200m (not the blue keyword colour).
  BOOST_CHECK(out.find("\x1b[38;2;100;200;200m") != std::string::npos);
  BOOST_CHECK(out.find("\x1b[38;2;80;130;200m") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(json_render_number_turquoise) {
  TB<TextBuffer> buf("test.json");
  buf.setMode(BufferMode::Jsn);
  insertString(buf, "42");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Number color: light turquoise \x1b[38;2;100;200;200m
  BOOST_CHECK(out.find("\x1b[38;2;100;200;200m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(json_goto_matching_bracket) {
  TB<TextBuffer> buf("test.json");
  buf.setMode(BufferMode::Jsn);
  insertString(buf, "[1, 2]");
  buf.moveHome(); // cursor on [
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 5u); // on ]
}

BOOST_AUTO_TEST_CASE(json_bracket_match_highlight_in_render) {
  TB<TextBuffer> buf("test.json");
  buf.setMode(BufferMode::Jsn);
  insertString(buf, "[1]");
  buf.moveHome(); // cursor on [
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40}, renderParams(buf));
  // Bracket match: turquoise \x1b[38;2;60;170;170m
  BOOST_CHECK(out.find("\x1b[38;2;60;170;170m") != std::string::npos);
}

// ============================================================
// Diff tokenizer
// ============================================================

BOOST_AUTO_TEST_CASE(diff_add_line) {
  auto line = mkline("+added line");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::DiffAdd));
}

BOOST_AUTO_TEST_CASE(diff_remove_line) {
  auto line = mkline("-removed line");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::DiffRemove));
}

BOOST_AUTO_TEST_CASE(diff_meta_plus_line) {
  auto line = mkline("+++ b/src/main.cpp");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::DiffMeta));
}

BOOST_AUTO_TEST_CASE(diff_meta_minus_line) {
  auto line = mkline("--- a/src/main.cpp");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::DiffMeta));
}

BOOST_AUTO_TEST_CASE(diff_hunk_header) {
  auto line = mkline("@@ -10,5 +10,7 @@ void foo()");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::DiffHunk));
}

BOOST_AUTO_TEST_CASE(diff_context_line) {
  auto line = mkline(" unchanged context");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(diff_empty_line) {
  auto line = mkline("");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(toks.empty());
}

BOOST_AUTO_TEST_CASE(diff_header_line) {
  auto line = mkline("diff --git a/foo b/foo");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(diff_plus_only) {
  auto line = mkline("+");
  std::vector<SyntaxToken> toks;
  tokenizeDiffLine(line, toks);
  BOOST_CHECK_EQUAL(toks.size(), 1u);
  BOOST_CHECK(toks[0] == SyntaxToken::DiffAdd);
}

BOOST_AUTO_TEST_CASE(diff_mode_detection) {
  TempFile tmp("", ".diff");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Dif);

  TempFile tmp2("", ".patch");
  TB<TextBuffer> buf2(tmp2.path().string(), tmp2.path());
  BOOST_CHECK(buf2.mode() == BufferMode::Dif);
}

// ============================================================
// Conflict marker detection
// ============================================================

BOOST_AUTO_TEST_CASE(conflict_detected_on_load) {
  TempFile tmp("line1\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\nline2\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.hasConflicts());
}

BOOST_AUTO_TEST_CASE(no_conflict_without_markers) {
  TempFile tmp("line1\nline2\nline3\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(!buf.hasConflicts());
}

BOOST_AUTO_TEST_CASE(conflict_needs_seven_chars) {
  TempFile tmp("<<<<<< HEAD\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(!buf.hasConflicts());
}

BOOST_AUTO_TEST_CASE(conflict_detected_with_equals) {
  TempFile tmp("=======\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.hasConflicts());
}

BOOST_AUTO_TEST_CASE(conflict_detected_on_revert) {
  TempFile tmp("clean\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(!buf.hasConflicts());
  // Rewrite file with conflict markers and revert.
  {
    std::ofstream f(tmp.path(), std::ios::binary);
    f << "<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n";
  }
  BOOST_CHECK(buf.revert());
  BOOST_CHECK(buf.hasConflicts());
}

BOOST_AUTO_TEST_CASE(conflict_tokens_override_cpp_mode) {
  TempFile tmp("return 0;\n<<<<<<< HEAD\nreturn 1;\n=======\nreturn 2;\n>>>>>>> fix\n", ".cpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Cpp);
  BOOST_CHECK(buf.hasConflicts());
  // Line 1 ("<<<<<<< HEAD") should have ConflictMarker tokens.
  auto toks = buf.cachedLineTokens(1);
  BOOST_CHECK(!toks.empty());
  BOOST_CHECK(toks[0] == SyntaxToken::ConflictMarker);
  // Line 0 ("return 0;") should still have normal C++ tokens.
  auto toks0 = buf.cachedLineTokens(0);
  BOOST_CHECK(toks0[0] == SyntaxToken::Keyword); // "return"
}

BOOST_AUTO_TEST_CASE(mode_change_retokenizes_with_the_new_modes_state) {
  // The cache holds one entry type per mode's state; switching to a mode
  // with another state type must start over, not reuse the old entries.
  TempFile tmp("/* a\nb */\n# x\n", ".cpp");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.cachedLineTokens(1)[0] == SyntaxToken::Comment); // block comment carried
  BOOST_CHECK(buf.cachedLineTokens(2)[0] == SyntaxToken::Preprocessor);

  buf.setMode(BufferMode::Sh);
  BOOST_CHECK(buf.cachedLineTokens(1)[0] == SyntaxToken::Normal);
  BOOST_CHECK(buf.cachedLineTokens(2)[0] == SyntaxToken::Comment);

  buf.setMode(BufferMode::Txt);
  BOOST_CHECK(buf.cachedLineTokens(2)[0] == SyntaxToken::Normal);

  buf.setMode(BufferMode::Cpp);
  BOOST_CHECK(buf.cachedLineTokens(1)[0] == SyntaxToken::Comment);
  BOOST_CHECK(buf.cachedLineTokens(2)[0] == SyntaxToken::Preprocessor);
}

BOOST_AUTO_TEST_CASE(conflict_tokens_in_txt_mode) {
  TempFile tmp("hello\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n", ".txt");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.mode() == BufferMode::Txt);
  BOOST_CHECK(buf.hasConflicts());
  // Conflict lines should still get ConflictMarker tokens even in Txt mode.
  auto toks = buf.cachedLineTokens(1);
  BOOST_CHECK(!toks.empty());
  BOOST_CHECK(toks[0] == SyntaxToken::ConflictMarker);
  // Non-conflict line should be Normal.
  auto toks0 = buf.cachedLineTokens(0);
  BOOST_CHECK(toks0[0] == SyntaxToken::Normal);
}

BOOST_AUTO_TEST_CASE(conflict_cleared_after_save) {
  TempFile tmp("<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> branch\n");
  TB<TextBuffer> buf(tmp.path().string(), tmp.path());
  BOOST_CHECK(buf.hasConflicts());
  // Replace entire buffer content with resolved text (no conflict markers).
  buf.deleteRegion({0, 0}, {buf.lineCount() - 1, buf.lineContent(buf.lineCount() - 1).size()});
  buf.setPoint(0, 0);
  insertString(buf, "resolved\n");
  BOOST_CHECK(buf.hasConflicts()); // still stale before save
  auto result = buf.save();
  BOOST_CHECK(result.has_value());
  BOOST_CHECK(!buf.hasConflicts()); // cleared after save
}

// ============================================================
// Shell (sh) mode
// ============================================================

BOOST_AUTO_TEST_CASE(sh_comment_after_space) {
  auto line = mkline("echo hi # note");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 8, line.size(), SyntaxToken::Comment));
  BOOST_CHECK(toks[0] == SyntaxToken::Normal); // 'echo' is not a keyword
}

BOOST_AUTO_TEST_CASE(sh_hash_without_space_is_not_comment) {
  auto line = mkline("a#b");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 3, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(sh_single_quoted_string) {
  auto line = mkline("x='a b'");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 2, 7, SyntaxToken::String));
  BOOST_CHECK(toks[1] == SyntaxToken::Operator); // '='
}

BOOST_AUTO_TEST_CASE(sh_double_quoted_string_with_escape) {
  auto line = mkline(R"(s="a\"b")");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  // Whole "a\"b" span (offsets 2..8) is String, escaped quote included.
  BOOST_CHECK(allTokens(toks, 2, 8, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(sh_backtick_is_string) {
  auto line = mkline("x=`date`");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 2, 8, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(sh_keywords_if_then) {
  auto line = mkline("if true; then");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 2, SyntaxToken::Keyword));  // if
  BOOST_CHECK(allTokens(toks, 9, 13, SyntaxToken::Keyword)); // then
  BOOST_CHECK(toks[7] == SyntaxToken::Operator);             // ';'
  BOOST_CHECK(toks[3] == SyntaxToken::Normal);               // 'true' is a command
}

BOOST_AUTO_TEST_CASE(sh_variable_simple) {
  auto line = mkline("echo $HOME");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 5, 10, SyntaxToken::Variable));
}

BOOST_AUTO_TEST_CASE(sh_variable_braced) {
  auto line = mkline("echo ${HOME}");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 5, 12, SyntaxToken::Variable));
}

BOOST_AUTO_TEST_CASE(sh_variable_special_param) {
  auto line = mkline("echo $?");
  ShSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeShellLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 5, 7, SyntaxToken::Variable));
}

BOOST_AUTO_TEST_CASE(sh_heredoc_open_body_close) {
  ShSyntaxState st;
  std::vector<SyntaxToken> t1, t2, t3;
  auto l1 = mkline("cat <<EOF");
  tokenizeShellLine(l1, st, t1);
  BOOST_CHECK(t1[4] == SyntaxToken::Operator && t1[5] == SyntaxToken::Operator);
  BOOST_CHECK(allTokens(t1, 6, 9, SyntaxToken::String)); // delimiter word
  BOOST_CHECK(st.heredocTag);

  auto l2 = mkline("  hello $x");
  tokenizeShellLine(l2, st, t2);
  BOOST_CHECK(allTokens(t2, 0, l2.size(), SyntaxToken::String)); // body
  BOOST_CHECK(st.heredocTag);                                    // still open

  auto l3 = mkline("EOF");
  tokenizeShellLine(l3, st, t3);
  BOOST_CHECK(allTokens(t3, 0, 3, SyntaxToken::Normal)); // terminator
  BOOST_CHECK(!st.heredocTag);                           // closed
}

BOOST_AUTO_TEST_CASE(sh_heredoc_fake_terminator_inside_is_body) {
  ShSyntaxState st;
  std::vector<SyntaxToken> t;
  auto l1 = mkline("cat <<END");
  tokenizeShellLine(l1, st, t);
  auto body = mkline("ENDX"); // not exactly the tag — still body
  tokenizeShellLine(body, st, t);
  BOOST_CHECK(allTokens(t, 0, 4, SyntaxToken::String));
  BOOST_CHECK(st.heredocTag);
}

BOOST_AUTO_TEST_CASE(sh_heredoc_dash_strips_leading_tabs) {
  ShSyntaxState st;
  std::vector<SyntaxToken> t;
  auto l1 = mkline("cat <<-END");
  tokenizeShellLine(l1, st, t);
  BOOST_CHECK(st.heredocTag);
  BOOST_CHECK(st.heredocStripTabs);
  auto term = mkline("\tEND"); // tab-indented terminator closes <<-
  tokenizeShellLine(term, st, t);
  BOOST_CHECK(!st.heredocTag);
}

BOOST_AUTO_TEST_CASE(sh_here_string_is_not_heredoc) {
  ShSyntaxState st;
  std::vector<SyntaxToken> t;
  auto line = mkline("cat <<<word");
  tokenizeShellLine(line, st, t);
  BOOST_CHECK(!st.heredocTag); // <<< is a here-string
}

// ============================================================
// YAML (yml) mode
// ============================================================

BOOST_AUTO_TEST_CASE(yml_key_and_string_value) {
  auto line = mkline(R"(name: "John")");
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeYamlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Keyword)); // name
  BOOST_CHECK(toks[4] == SyntaxToken::Operator);            // ':'
  BOOST_CHECK(allTokens(toks, 6, 12, SyntaxToken::String)); // "John"
}

BOOST_AUTO_TEST_CASE(yml_bool_and_number_scalars) {
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("enabled: true");
  tokenizeYamlLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::Keyword)); // 'enabled' key
  BOOST_CHECK(allTokens(toks, 9, 13, SyntaxToken::Number)); // true (literal value)
  auto l2 = mkline("count: -12.5");
  tokenizeYamlLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 7, 12, SyntaxToken::Number)); // -12.5
}

BOOST_AUTO_TEST_CASE(yml_comment_after_space) {
  auto line = mkline("port: 80 # http");
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeYamlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 9, line.size(), SyntaxToken::Comment));
  BOOST_CHECK(allTokens(toks, 6, 8, SyntaxToken::Number)); // 80
}

BOOST_AUTO_TEST_CASE(yml_sequence_bullet_is_operator) {
  auto line = mkline("  - item");
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeYamlLine(line, st, toks);
  BOOST_CHECK(toks[2] == SyntaxToken::Operator); // '-'
  BOOST_CHECK(toks[4] == SyntaxToken::Normal);   // plain scalar 'item'
}

BOOST_AUTO_TEST_CASE(yml_anchor_and_doc_marker) {
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto doc = mkline("---");
  tokenizeYamlLine(doc, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 3, SyntaxToken::Preprocessor));
  auto anc = mkline("base: &a value");
  tokenizeYamlLine(anc, st, toks);
  BOOST_CHECK(allTokens(toks, 6, 8, SyntaxToken::Preprocessor)); // &a
}

BOOST_AUTO_TEST_CASE(yml_block_scalar_spans_until_dedent) {
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("script: |");
  tokenizeYamlLine(l1, st, toks);
  BOOST_CHECK(toks[8] == SyntaxToken::Operator); // '|'
  BOOST_CHECK(st.yamlBlockCol == 0);

  auto l2 = mkline("  echo hi: not a key");
  tokenizeYamlLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::String)); // body
  BOOST_CHECK(st.yamlBlockCol == 0);                               // still open

  auto blank = mkline("");
  tokenizeYamlLine(blank, st, toks);
  BOOST_CHECK(st.yamlBlockCol == 0); // blank line stays in block

  auto l3 = mkline("next: 1");
  tokenizeYamlLine(l3, st, toks);
  BOOST_CHECK(st.yamlBlockCol == -1);                       // dedent closes
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Keyword)); // parsed normally
}

BOOST_AUTO_TEST_CASE(yml_colon_without_space_is_not_key) {
  auto line = mkline("http://example.com");
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeYamlLine(line, st, toks);
  // No "<key>: " so nothing is a Keyword; URL stays a plain scalar.
  BOOST_CHECK(toks[0] == SyntaxToken::Normal);
  BOOST_CHECK(toks[4] == SyntaxToken::Normal); // the ':' in http:
}

// Regression: a '#' directly after a flow indicator or closing quote used
// to stop the plain-scalar scan without consuming anything, hanging the
// tokenizer (and the editor) forever.
BOOST_AUTO_TEST_CASE(yml_hash_without_preceding_space_terminates) {
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  for (auto const* s : {"colors: [#fff, #000]", R"(a: "x"#y)", "a: {k: v}#c", "a: b,#c"}) {
    auto line = mkline(s);
    tokenizeYamlLine(line, st, toks);
    BOOST_CHECK_EQUAL(toks.size(), line.size());
  }
  auto line = mkline("colors: [#fff]");
  tokenizeYamlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 9, 13, SyntaxToken::Normal)); // '#fff' is content
}

BOOST_AUTO_TEST_CASE(yml_hash_inside_key_is_not_comment) {
  auto line = mkline("c#sharp: 1");
  YmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeYamlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::Keyword));
}

// ============================================================
// TOML (tml) mode
// ============================================================

BOOST_AUTO_TEST_CASE(tml_key_value_and_comment) {
  auto line = mkline(R"(name = "cx" # editor)");
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeTomlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Keyword));            // name
  BOOST_CHECK(toks[5] == SyntaxToken::Operator);                       // '='
  BOOST_CHECK(allTokens(toks, 7, 11, SyntaxToken::String));            // "cx"
  BOOST_CHECK(allTokens(toks, 12, line.size(), SyntaxToken::Comment)); // # editor
}

BOOST_AUTO_TEST_CASE(tml_dotted_and_quoted_keys) {
  auto line = mkline(R"(site."google.com".x = 1)");
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeTomlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Keyword)); // site
  BOOST_CHECK(toks[4] == SyntaxToken::Operator);            // '.'
  BOOST_CHECK(allTokens(toks, 5, 17, SyntaxToken::String)); // "google.com"
  BOOST_CHECK(toks[18] == SyntaxToken::Keyword);            // x
  BOOST_CHECK(toks[22] == SyntaxToken::Number);             // 1
}

BOOST_AUTO_TEST_CASE(tml_table_headers) {
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto t = mkline("[server.http] # main");
  tokenizeTomlLine(t, st, toks);
  BOOST_CHECK(toks[0] == SyntaxToken::Bracket);
  BOOST_CHECK(allTokens(toks, 1, 7, SyntaxToken::Preprocessor)); // server
  BOOST_CHECK(toks[7] == SyntaxToken::Operator);                 // '.'
  BOOST_CHECK(toks[12] == SyntaxToken::Bracket);
  BOOST_CHECK(allTokens(toks, 14, t.size(), SyntaxToken::Comment));
  auto a = mkline("[[bin]]");
  tokenizeTomlLine(a, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 2, SyntaxToken::Bracket));
  BOOST_CHECK(allTokens(toks, 2, 5, SyntaxToken::Preprocessor));
  BOOST_CHECK(allTokens(toks, 5, 7, SyntaxToken::Bracket));
  BOOST_CHECK(st.tomlDepth == 0);
}

BOOST_AUTO_TEST_CASE(tml_literal_values) {
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  for (auto const* v :
       {"true", "false", "-17", "1_000", "0xDEAD_beef", "0o755", "0b1101", "+1.5e-3", "-inf", "nan",
        "1979-05-27", "07:32:00", "1979-05-27T07:32:00Z", "1979-05-27 07:32:00.999-07:00"}) {
    auto line = "k = " + std::string(v);
    tokenizeTomlLine(line, st, toks);
    BOOST_CHECK_MESSAGE(allTokens(toks, 4, line.size(), SyntaxToken::Number), v);
  }
  for (auto const* v : {"True", "0x", "1.", "abc", "1__x"}) {
    auto line = "k = " + std::string(v);
    tokenizeTomlLine(line, st, toks);
    BOOST_CHECK_MESSAGE(toks[4] == SyntaxToken::Normal, v);
  }
}

BOOST_AUTO_TEST_CASE(tml_hash_inside_string_is_not_comment) {
  auto line = mkline(R"(color = "#fff" # c)");
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeTomlLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 8, 14, SyntaxToken::String));
  BOOST_CHECK(allTokens(toks, 15, line.size(), SyntaxToken::Comment));
}

BOOST_AUTO_TEST_CASE(tml_inline_table_keys) {
  auto line = mkline("p = { x = 1, y.z = 'a' }");
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizeTomlLine(line, st, toks);
  BOOST_CHECK(toks[4] == SyntaxToken::Brace);
  BOOST_CHECK(toks[6] == SyntaxToken::Keyword);   // x
  BOOST_CHECK(toks[10] == SyntaxToken::Number);   // 1
  BOOST_CHECK(toks[11] == SyntaxToken::Operator); // ','
  BOOST_CHECK(toks[13] == SyntaxToken::Keyword);  // y
  BOOST_CHECK(toks[15] == SyntaxToken::Keyword);  // z
  BOOST_CHECK(allTokens(toks, 19, 22, SyntaxToken::String));
  BOOST_CHECK(toks[23] == SyntaxToken::Brace);
  BOOST_CHECK(st.tomlDepth == 0);
}

BOOST_AUTO_TEST_CASE(tml_multiline_array_is_not_header) {
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline("data = [");
  tokenizeTomlLine(l1, st, toks);
  BOOST_CHECK(st.tomlDepth == 1);
  auto l2 = mkline("  [1, 2], # pair");
  tokenizeTomlLine(l2, st, toks);
  BOOST_CHECK(toks[2] == SyntaxToken::Bracket);
  BOOST_CHECK(toks[3] == SyntaxToken::Number); // value, not a header name
  BOOST_CHECK(allTokens(toks, 10, l2.size(), SyntaxToken::Comment));
  auto l3 = mkline("  a,");
  tokenizeTomlLine(l3, st, toks);
  BOOST_CHECK(toks[2] == SyntaxToken::Normal); // no key inside an array
  auto l4 = mkline("]");
  tokenizeTomlLine(l4, st, toks);
  BOOST_CHECK(st.tomlDepth == 0);
  auto l5 = mkline("next = 1");
  tokenizeTomlLine(l5, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Keyword));
}

BOOST_AUTO_TEST_CASE(tml_multiline_strings) {
  TmlSyntaxState st;
  std::vector<SyntaxToken> toks;
  auto l1 = mkline(R"(s = """first)");
  tokenizeTomlLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 4, l1.size(), SyntaxToken::String));
  BOOST_CHECK(st.tomlTriple == '"');
  auto l2 = mkline(R"(not = a key \""" still)");
  tokenizeTomlLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::String)); // escaped quote
  BOOST_CHECK(st.tomlTriple == '"');
  auto l3 = mkline(R"(end"""" # c)");
  tokenizeTomlLine(l3, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::String)); // `end"` + delimiter
  BOOST_CHECK(allTokens(toks, 8, l3.size(), SyntaxToken::Comment));
  BOOST_CHECK(st.tomlTriple == 0);
  auto l4 = mkline(R"(p = '''C:\x''' )");
  tokenizeTomlLine(l4, st, toks);
  BOOST_CHECK(allTokens(toks, 4, 14, SyntaxToken::String)); // literal: no escapes
  BOOST_CHECK(st.tomlTriple == 0);
}

// ============================================================
// Python (py) mode
// ============================================================

BOOST_AUTO_TEST_CASE(py_comment_and_number) {
  auto line = mkline("x = 1  # note");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  BOOST_CHECK(toks[4] == SyntaxToken::Number); // 1
  BOOST_CHECK(allTokens(toks, 7, line.size(), SyntaxToken::Comment));
}

BOOST_AUTO_TEST_CASE(py_keyword_def_and_punctuation) {
  auto line = mkline("def foo():");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 3, SyntaxToken::Keyword)); // def
  BOOST_CHECK(toks[7] == SyntaxToken::Paren);               // (
  BOOST_CHECK(toks[9] == SyntaxToken::Operator);            // :
}

BOOST_AUTO_TEST_CASE(py_value_literals_are_number) {
  auto line = mkline("x = None or True and False");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 4, 8, SyntaxToken::Number));    // None
  BOOST_CHECK(allTokens(toks, 9, 11, SyntaxToken::Keyword));  // or
  BOOST_CHECK(allTokens(toks, 12, 16, SyntaxToken::Number));  // True
  BOOST_CHECK(allTokens(toks, 17, 20, SyntaxToken::Keyword)); // and
  BOOST_CHECK(allTokens(toks, 21, 26, SyntaxToken::Number));  // False
}

BOOST_AUTO_TEST_CASE(py_fstring_keeps_braces_in_string) {
  auto line = mkline(R"(f"a{x}b")");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  // Prefix, quotes and the {x} interior are all one String span.
  BOOST_CHECK(allTokens(toks, 0, line.size(), SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(py_number_hex_and_imaginary) {
  auto line = mkline("0xFF + 3j");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 4, SyntaxToken::Number)); // 0xFF
  BOOST_CHECK(allTokens(toks, 7, 9, SyntaxToken::Number)); // 3j
}

BOOST_AUTO_TEST_CASE(py_decorator_only_at_line_start) {
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  auto dec = mkline("@app.route");
  tokenizePythonLine(dec, st, toks);
  BOOST_CHECK(allTokens(toks, 0, dec.size(), SyntaxToken::Preprocessor));
  auto mid = mkline("x @ y"); // '@' not first token -> not a decorator
  tokenizePythonLine(mid, st, toks);
  BOOST_CHECK(toks[2] == SyntaxToken::Normal);
}

BOOST_AUTO_TEST_CASE(py_triple_quoted_spans_lines_with_fake_close_inside) {
  PySyntaxState st;
  std::vector<SyntaxToken> toks;

  auto l1 = mkline(R"(s = """start)");
  tokenizePythonLine(l1, st, toks);
  BOOST_CHECK(allTokens(toks, 4, l1.size(), SyntaxToken::String));
  BOOST_CHECK(st.pyTriple == '"'); // open across lines

  auto l2 = mkline(R"(has "fake" but not a close)");
  tokenizePythonLine(l2, st, toks);
  BOOST_CHECK(allTokens(toks, 0, l2.size(), SyntaxToken::String));
  BOOST_CHECK(st.pyTriple == '"'); // single quotes don't close it

  auto l3 = mkline(R"(end""" + x)");
  tokenizePythonLine(l3, st, toks);
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::String)); // end + closing """
  BOOST_CHECK(st.pyTriple == 0);                           // closed
  BOOST_CHECK(toks[9] == SyntaxToken::Normal);             // code after string
}

BOOST_AUTO_TEST_CASE(py_prefix_letter_word_is_not_a_string) {
  auto line = mkline("understand = 1");
  PySyntaxState st;
  std::vector<SyntaxToken> toks;
  tokenizePythonLine(line, st, toks);
  // 'u' is a string prefix letter, but "understand" is an identifier.
  BOOST_CHECK(toks[0] == SyntaxToken::Normal);
  BOOST_CHECK(toks[13] == SyntaxToken::Number); // the 1
}

BOOST_AUTO_TEST_CASE(every_c_keyword_is_highlighted) {
  // isKeyword() binary-searches the table; "continue" sorted before
  // "co_await" hid const, constexpr, continue, co_await and friends.
  static constexpr std::string_view kAll[] = {
      "alignas",
      "alignof",
      "auto",
      "break",
      "case",
      "catch",
      "class",
      "co_await",
      "co_return",
      "co_yield",
      "const",
      "const_cast",
      "consteval",
      "constexpr",
      "constinit",
      "continue",
      "decltype",
      "default",
      "delete",
      "do",
      "dynamic_cast",
      "else",
      "enum",
      "explicit",
      "export",
      "extern",
      "for",
      "friend",
      "goto",
      "if",
      "inline",
      "mutable",
      "namespace",
      "new",
      "noexcept",
      "operator",
      "override",
      "private",
      "protected",
      "public",
      "reinterpret_cast",
      "requires",
      "return",
      "sizeof",
      "static",
      "static_assert",
      "static_cast",
      "struct",
      "switch",
      "template",
      "this",
      "throw",
      "try",
      "typedef",
      "typeid",
      "typename",
      "union",
      "using",
      "virtual",
      "volatile",
      "while",
  };
  for (auto kw : kAll) {
    auto line = mkline(kw);
    CSyntaxState st;
    std::vector<SyntaxToken> toks;
    tokenizeLine(line, st, toks);
    BOOST_CHECK_MESSAGE(allTokens(toks, 0, kw.size(), SyntaxToken::Keyword), kw);
  }
}
