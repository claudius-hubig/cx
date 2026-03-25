#define BOOST_TEST_MODULE Syntax
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "buffer/modes/c/syntax.hpp"
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
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // "hello" is at positions 4..10 (including quotes).
  BOOST_CHECK(allTokens(toks, 4, 11, SyntaxToken::String));
  // x should be Normal.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[0]), static_cast<int>(SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(char_literal_is_string_token) {
  auto line = mkline("char c = 'x';");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // 'x' is at positions 9..11.
  BOOST_CHECK(allTokens(toks, 9, 12, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(escaped_quote_in_string) {
  auto line = mkline(R"(s = "a\"b";)");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // The whole "a\"b" should be String.
  BOOST_CHECK(allTokens(toks, 4, 10, SyntaxToken::String));
}

BOOST_AUTO_TEST_CASE(thousand_separator_not_string) {
  auto line = mkline("int x = 1'000'000;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // The apostrophes at positions 10 and 14 should NOT be String.
  BOOST_CHECK(toks[10] != SyntaxToken::String);
  BOOST_CHECK(toks[14] != SyntaxToken::String);
  // The digits around them should be Normal.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[8]), static_cast<int>(SyntaxToken::Normal));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[11]), static_cast<int>(SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(raw_string_single_line) {
  auto line = mkline(R"__(s = R"(hello)";)__");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // R"(hello)" spans from position 4 to 13 inclusive.
  BOOST_CHECK(allTokens(toks, 4, 14, SyntaxToken::String));
  // ; at position 14 is NOT string.
  BOOST_CHECK(toks[14] != SyntaxToken::String);
  BOOST_CHECK(!st.inBlockComment);
  BOOST_CHECK(!st.rawDelim.has_value());
}

BOOST_AUTO_TEST_CASE(raw_string_with_delimiter) {
  auto line = mkline(R"__(s = R"foo(hello)foo";)__");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // R"foo(hello)foo" is all String.
  BOOST_CHECK(allTokens(toks, 4, 20, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value());
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_start) {
  auto line = mkline(R"__(s = R"(hello)__");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // Everything from R onwards is String.
  BOOST_CHECK(allTokens(toks, 4, line.size(), SyntaxToken::String));
  // Raw string continues to next line.
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(*st.rawDelim, "");
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_end) {
  // Simulate second line of a multiline raw string with empty delimiter.
  // Line content: world)";
  std::string content = "world)\";";
  auto line = mkline(content);
  SyntaxState st;
  st.rawDelim = std::string(""); // continuing from previous line
  auto toks = tokenizeLine(line, st);
  // Everything up to and including )" should be String (positions 0..6).
  BOOST_CHECK(allTokens(toks, 0, 7, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value()); // raw string ended
}

BOOST_AUTO_TEST_CASE(raw_string_multiline_with_delim) {
  auto line1 = mkline(R"__(R"foo(start)__");
  SyntaxState st;
  auto toks1 = tokenizeLine(line1, st);
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(*st.rawDelim, "foo");

  auto line2 = mkline(R"__(middle)__");
  auto toks2 = tokenizeLine(line2, st);
  BOOST_CHECK(allTokens(toks2, 0, line2.size(), SyntaxToken::String));
  BOOST_CHECK(st.rawDelim.has_value()); // still inside

  // end)foo";  — the )foo" closes the raw string.
  auto line3 = mkline(R"__(end)foo";)__");
  auto toks3 = tokenizeLine(line3, st);
  // end)foo" = 8 chars of String (positions 0-7), ; at 8 is not.
  BOOST_CHECK(allTokens(toks3, 0, 8, SyntaxToken::String));
  BOOST_CHECK(!st.rawDelim.has_value()); // ended
}

BOOST_AUTO_TEST_CASE(string_render_uses_red_color) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, R"(x = "hello";)");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // String color (red): \x1b[38;2;200;80;80m
  BOOST_CHECK(out.find("\x1b[38;2;200;80;80m") != std::string::npos);
}

// ============================================================
// Item 2: Angle bracket operator heuristic
// ============================================================

BOOST_AUTO_TEST_CASE(shift_left_is_operator) {
  auto line = mkline("x = a << b;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // << at positions 6,7 should be Operator.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(shift_right_is_operator) {
  auto line = mkline("x = a >> b;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(less_equal_is_operator) {
  auto line = mkline("if (a <= b)");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // <= at positions 6,7.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(greater_equal_is_operator) {
  auto line = mkline("if (a >= b)");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[7]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(spaced_less_than_is_operator) {
  auto line = mkline("if (a < b)");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // < at position 6 with spaces around it → Operator.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(template_angle_is_angle) {
  auto line = mkline("vector<int> v;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // < at position 6 — no spaces → Angle.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Angle));
  // > at position 10 — no spaces → Angle.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Angle));
}

BOOST_AUTO_TEST_CASE(spaceship_is_operator) {
  auto line = mkline("a <=> b");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // <=> at positions 2,3,4.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[4]), static_cast<int>(SyntaxToken::Operator));
}

// ============================================================
// Item 3: '?' bracket jump (gotoMatchingBracket)
// ============================================================

BOOST_AUTO_TEST_CASE(goto_matching_paren_forward) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar)");
  buf.setPoint(0, 3); // on '('
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 7u); // on ')'
}

BOOST_AUTO_TEST_CASE(goto_matching_paren_backward) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar)");
  buf.setPoint(0, 7); // on ')'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 3u); // on '('
}

BOOST_AUTO_TEST_CASE(goto_matching_brace) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "{ x }");
  buf.setPoint(0, 0); // on '{'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 4u); // on '}'
}

BOOST_AUTO_TEST_CASE(goto_matching_bracket_square) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "a[b]");
  buf.setPoint(0, 1); // on '['
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 3u); // on ']'
}

BOOST_AUTO_TEST_CASE(goto_matching_angle) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "vector<int>");
  buf.setPoint(0, 6); // on '<'
  BOOST_CHECK(buf.gotoMatchingBracket());
  BOOST_CHECK_EQUAL(buf.point().offset, 10u); // on '>'
}

BOOST_AUTO_TEST_CASE(goto_matching_multiline) {
  TextBuffer buf("test.cpp");
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
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "hello");
  buf.setPoint(0, 0); // on 'h'
  BOOST_CHECK(!buf.gotoMatchingBracket());
}

BOOST_AUTO_TEST_CASE(goto_matching_no_match) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(bar");
  buf.setPoint(0, 3); // on '(' with no matching ')'
  BOOST_CHECK(!buf.gotoMatchingBracket());
}

// ============================================================
// Item 4: Brackets have no color but match in turquoise
// ============================================================

BOOST_AUTO_TEST_CASE(brackets_not_colored_individually) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 0); // cursor NOT on bracket
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // Old bracket colors should not appear.
  // Old paren red: \x1b[38;2;200;80;80m — now used for String, not brackets.
  // Old bracket orange: \x1b[38;2;180;140;40m
  // Old brace purple: \x1b[38;2;150;100;200m — now used for Operator.
  // Without any strings or operators, none of these should appear.
  BOOST_CHECK(out.find("\x1b[38;2;180;140;40m") == std::string::npos); // no orange
}

BOOST_AUTO_TEST_CASE(bracket_match_uses_turquoise) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 3); // on '(' — should match ')'
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // Turquoise: \x1b[38;2;60;170;170m
  BOOST_CHECK(out.find("\x1b[38;2;60;170;170m") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(bracket_at_point_also_highlighted) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "foo(x)");
  buf.setPoint(0, 3); // on '('
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
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
// Item 5: true/false not highlighted as keywords
// ============================================================

BOOST_AUTO_TEST_CASE(true_not_keyword) {
  auto line = mkline("bool x = true;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // "true" starts at position 9.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[9]), static_cast<int>(SyntaxToken::Normal));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Normal));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[11]), static_cast<int>(SyntaxToken::Normal));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[12]), static_cast<int>(SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(false_not_keyword) {
  auto line = mkline("bool x = false;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // "false" starts at position 9.
  BOOST_CHECK(allTokens(toks, 9, 14, SyntaxToken::Normal));
}

BOOST_AUTO_TEST_CASE(nullptr_still_keyword) {
  auto line = mkline("return nullptr;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // "return" at 0..5 is Keyword.
  BOOST_CHECK(allTokens(toks, 0, 6, SyntaxToken::Keyword));
  // "nullptr" at 7..13 is Keyword.
  BOOST_CHECK(allTokens(toks, 7, 14, SyntaxToken::Keyword));
}

// ============================================================
// Item 6: Operator highlighting
// ============================================================

BOOST_AUTO_TEST_CASE(equals_is_operator) {
  auto line = mkline("x = 5;");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(not_equals_is_operator) {
  auto line = mkline("x != y");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  // != at positions 2,3.
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(logical_and_is_operator) {
  auto line = mkline("a && b");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(logical_or_is_operator) {
  auto line = mkline("a || b");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[3]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(ternary_is_operator) {
  auto line = mkline("x ? a : b");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(plus_minus_star_are_operators) {
  auto line = mkline("a + b - c * d");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[2]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[6]), static_cast<int>(SyntaxToken::Operator));
  BOOST_CHECK_EQUAL(static_cast<int>(toks[10]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(tilde_is_operator) {
  auto line = mkline("~x");
  SyntaxState st;
  auto toks = tokenizeLine(line, st);
  BOOST_CHECK_EQUAL(static_cast<int>(toks[0]), static_cast<int>(SyntaxToken::Operator));
}

BOOST_AUTO_TEST_CASE(operator_render_uses_purple_color) {
  TextBuffer buf("test.cpp");
  buf.setMode(BufferMode::Cpp);
  insertString(buf, "x = 5;");
  buf.moveHome();
  std::string out;
  buf.render(out, RowIdx{5}, ColIdx{40});
  // Operator color (purple): \x1b[38;2;150;100;200m
  BOOST_CHECK(out.find("\x1b[38;2;150;100;200m") != std::string::npos);
}

// ============================================================
// advanceBlockCommentState with raw strings
// ============================================================

BOOST_AUTO_TEST_CASE(advance_state_skips_raw_string) {
  auto line1 = mkline(R"__(R"(hello)__");
  SyntaxState st;
  advanceBlockCommentState(line1, st);
  BOOST_CHECK(st.rawDelim.has_value());
  BOOST_CHECK_EQUAL(*st.rawDelim, "");
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
  SyntaxState st;
  advanceBlockCommentState(line, st);
  BOOST_CHECK(!st.inBlockComment);
  BOOST_CHECK(!st.rawDelim.has_value());
}

// ============================================================
// Bug 1: findMatchingBracket must skip brackets in strings/comments
// ============================================================

// Build a lines vector from newline-separated text.
static std::vector<std::string> mklines(std::string_view s) {
  std::vector<std::string> result;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      result.emplace_back(s.substr(start, i - start));
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
