#define BOOST_TEST_MODULE KeySequence
#include <boost/test/unit_test.hpp>

#include "key_sequence.hpp"

#include <string_view>

namespace {

bool isKey(std::optional<KeyPress> got, KeyPress want) { return got && *got == want; }

} // namespace

BOOST_AUTO_TEST_CASE(letter_finals_name_the_arrows_and_home_end) {
  BOOST_CHECK(isKey(decodeCsiKey("", 'A'), {Key::Up}));
  BOOST_CHECK(isKey(decodeCsiKey("", 'B'), {Key::Down}));
  BOOST_CHECK(isKey(decodeCsiKey("", 'C'), {Key::Right}));
  BOOST_CHECK(isKey(decodeCsiKey("", 'D'), {Key::Left}));
  BOOST_CHECK(isKey(decodeCsiKey("", 'H'), {Key::Home}));
  BOOST_CHECK(isKey(decodeCsiKey("", 'F'), {Key::End}));
  // An explicit 1 is the same key.
  BOOST_CHECK(isKey(decodeCsiKey("1", 'A'), {Key::Up}));
}

BOOST_AUTO_TEST_CASE(tilde_finals_name_the_key_by_number) {
  BOOST_CHECK(isKey(decodeCsiKey("2", '~'), {Key::Insert}));
  BOOST_CHECK(isKey(decodeCsiKey("3", '~'), {Key::Delete}));
  BOOST_CHECK(isKey(decodeCsiKey("5", '~'), {Key::PageUp}));
  BOOST_CHECK(isKey(decodeCsiKey("6", '~'), {Key::PageDown}));
}

BOOST_AUTO_TEST_CASE(home_and_end_in_the_vt220_and_rxvt_forms) {
  // tmux, screen and the Linux console send ESC[1~ / ESC[4~; rxvt ESC[7~ / ESC[8~.
  BOOST_CHECK(isKey(decodeCsiKey("1", '~'), {Key::Home}));
  BOOST_CHECK(isKey(decodeCsiKey("7", '~'), {Key::Home}));
  BOOST_CHECK(isKey(decodeCsiKey("4", '~'), {Key::End}));
  BOOST_CHECK(isKey(decodeCsiKey("8", '~'), {Key::End}));
}

BOOST_AUTO_TEST_CASE(modifier_field_is_one_plus_a_bit_mask) {
  BOOST_CHECK(isKey(decodeCsiKey("1;2", 'C'), {.key = Key::Right, .shift = true}));
  BOOST_CHECK(isKey(decodeCsiKey("1;3", 'C'), {.key = Key::Right, .alt = true}));
  BOOST_CHECK(isKey(decodeCsiKey("1;5", 'C'), {.key = Key::Right, .ctrl = true}));
  BOOST_CHECK(isKey(decodeCsiKey("1;6", 'A'), {.key = Key::Up, .shift = true, .ctrl = true}));
  BOOST_CHECK(isKey(decodeCsiKey("3;5", '~'), {.key = Key::Delete, .ctrl = true}));
  BOOST_CHECK(isKey(decodeCsiKey("1;8", 'H'),
                    {.key = Key::Home, .shift = true, .alt = true, .ctrl = true}));

  auto ctrl = *decodeCsiKey("1;5", 'C');
  BOOST_CHECK(ctrl.ctrlOnly() && !ctrl.plain() && !ctrl.ctrlShiftOnly());
  auto ctrlShift = *decodeCsiKey("1;6", 'A');
  BOOST_CHECK(ctrlShift.ctrlShiftOnly() && !ctrlShift.ctrlOnly());
  BOOST_CHECK(decodeCsiKey("", 'A')->plain());
}

BOOST_AUTO_TEST_CASE(other_sequences_are_not_keys) {
  // Function keys: F5 = ESC[15~, F9 = ESC[20~, F12 = ESC[24~.
  BOOST_CHECK(!decodeCsiKey("15", '~'));
  BOOST_CHECK(!decodeCsiKey("20", '~'));
  BOOST_CHECK(!decodeCsiKey("24", '~'));
  // Shift-Tab, focus in/out, the Linux console's ESC[[A.
  BOOST_CHECK(!decodeCsiKey("", 'Z'));
  BOOST_CHECK(!decodeCsiKey("", 'I'));
  BOOST_CHECK(!decodeCsiKey("", '['));
  // A letter key takes no number but 1.
  BOOST_CHECK(!decodeCsiKey("5", 'A'));
  // Malformed parameters and out-of-range modifiers.
  BOOST_CHECK(isKey(decodeCsiKey("1;", 'A'), {Key::Up})); // an absent field is 1
  BOOST_CHECK(!decodeCsiKey("1;0", 'A'));
  BOOST_CHECK(!decodeCsiKey("1;9", 'A'));
  BOOST_CHECK(!decodeCsiKey("1;5;2", 'A'));
  BOOST_CHECK(!decodeCsiKey("<0;1;1", 'M'));
  BOOST_CHECK(!decodeCsiKey("?1", 'A'));
  BOOST_CHECK(!decodeCsiKey("99999999999999999999", '~'));
}

BOOST_AUTO_TEST_CASE(ss3_finals) {
  BOOST_CHECK(isKey(decodeSs3Key('A'), {Key::Up}));
  BOOST_CHECK(isKey(decodeSs3Key('D'), {Key::Left}));
  BOOST_CHECK(isKey(decodeSs3Key('H'), {Key::Home}));
  BOOST_CHECK(isKey(decodeSs3Key('F'), {Key::End}));
  // F1..F4 are ESC O P .. ESC O S.
  for (char c : std::string_view("PQRS"))
    BOOST_CHECK(!decodeSs3Key(c));
}

BOOST_AUTO_TEST_CASE(csi_final_byte_range) {
  BOOST_CHECK(isCsiFinal('@') && isCsiFinal('~') && isCsiFinal('A') && isCsiFinal('['));
  BOOST_CHECK(!isCsiFinal('0') && !isCsiFinal(';') && !isCsiFinal('<') && !isCsiFinal(' '));
  BOOST_CHECK(!isCsiFinal('\x7f'));
}
