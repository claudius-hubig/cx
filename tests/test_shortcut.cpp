#define BOOST_TEST_MODULE Shortcut
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include "shortcut.hpp"

// --- handleShortcut: forward lookup ---

BOOST_AUTO_TEST_CASE(ctrl_s_maps_to_save) {
  auto action = handleShortcut({.ctrl = true, .key = 's'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Save);
}

BOOST_AUTO_TEST_CASE(esc_question_maps_to_help) {
  auto action = handleShortcut({.esc = true, .key = '?'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Help);
}

BOOST_AUTO_TEST_CASE(ctrl_f_maps_to_find) {
  auto action = handleShortcut({.ctrl = true, .key = 'f'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Find);
}

BOOST_AUTO_TEST_CASE(ctrl_r_maps_to_find_backward) {
  auto action = handleShortcut({.ctrl = true, .key = 'r'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::FindBackward);
}

BOOST_AUTO_TEST_CASE(esc_r_maps_to_revert) {
  auto action = handleShortcut({.esc = true, .key = 'r'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Revert);
}

BOOST_AUTO_TEST_CASE(esc_p_maps_to_paste_ring) {
  auto action = handleShortcut({.esc = true, .key = 'p'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::PasteRing);
}

BOOST_AUTO_TEST_CASE(esc_c_maps_to_exit) {
  auto action = handleShortcut({.esc = true, .key = 'c'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Exit);
}

BOOST_AUTO_TEST_CASE(ctrl_space_maps_to_toggle_mark) {
  auto action = handleShortcut({.ctrl = true, .key = ' '});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::ToggleMark);
}

BOOST_AUTO_TEST_CASE(ctrl_h_maps_to_delete_chunk_before) {
  auto action = handleShortcut({.ctrl = true, .key = 'h'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::DeleteChunkBefore);
}

BOOST_AUTO_TEST_CASE(esc_backspace_maps_to_delete_chunk_before) {
  auto action = handleShortcut({.esc = true, .key = 0x7F});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::DeleteChunkBefore);
}

BOOST_AUTO_TEST_CASE(esc_m_maps_to_set_mode) {
  auto action = handleShortcut({.esc = true, .key = 'm'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::SetMode);
}

BOOST_AUTO_TEST_CASE(ctrl_p_maps_to_pipe_replace) {
  auto action = handleShortcut({.ctrl = true, .key = 'p'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::PipeReplace);
}

BOOST_AUTO_TEST_CASE(esc_v_maps_to_split_vertical) {
  auto action = handleShortcut({.esc = true, .key = 'v'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::SplitVertical);
}

BOOST_AUTO_TEST_CASE(esc_dot_maps_to_close_panel) {
  auto action = handleShortcut({.esc = true, .key = '.'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::ClosePanel);
}

BOOST_AUTO_TEST_CASE(esc_1_maps_to_focus_panel_1) {
  auto action = handleShortcut({.esc = true, .key = '1'});
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::FocusPanel1);
}

BOOST_AUTO_TEST_CASE(unrecognised_shortcut_returns_nullopt) {
  // Ctrl-z is not bound.
  auto action = handleShortcut({.ctrl = true, .key = 'z'});
  BOOST_CHECK(!action.has_value());
}

BOOST_AUTO_TEST_CASE(unrecognised_esc_returns_nullopt) {
  // Esc-x is not bound.
  auto action = handleShortcut({.esc = true, .key = 'x'});
  BOOST_CHECK(!action.has_value());
}

BOOST_AUTO_TEST_CASE(plain_key_returns_nullopt) {
  // No modifier — not a shortcut.
  auto action = handleShortcut({.key = 'a'});
  BOOST_CHECK(!action.has_value());
}

// --- shortcutForAction: reverse lookup ---

BOOST_AUTO_TEST_CASE(reverse_lookup_save) {
  auto s = shortcutForAction(Action::Save);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK(!s.esc);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('s'));
}

BOOST_AUTO_TEST_CASE(reverse_lookup_find) {
  auto s = shortcutForAction(Action::Find);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('f'));
}

BOOST_AUTO_TEST_CASE(reverse_lookup_revert) {
  auto s = shortcutForAction(Action::Revert);
  BOOST_CHECK(s.esc);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('r'));
}

BOOST_AUTO_TEST_CASE(reverse_lookup_find_backward) {
  auto s = shortcutForAction(Action::FindBackward);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('r'));
}

BOOST_AUTO_TEST_CASE(reverse_lookup_delete_chunk_returns_first) {
  // DeleteChunkBefore has two bindings; shortcutForAction returns the first (Ctrl-h).
  auto s = shortcutForAction(Action::DeleteChunkBefore);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('h'));
}

BOOST_AUTO_TEST_CASE(reverse_lookup_set_mode) {
  auto s = shortcutForAction(Action::SetMode);
  BOOST_CHECK(s.esc);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('m'));
}

// --- formatShortcut ---

BOOST_AUTO_TEST_CASE(format_ctrl_s) {
  BOOST_CHECK_EQUAL(formatShortcut({.ctrl = true, .key = 's'}), "Ctrl-s");
}

BOOST_AUTO_TEST_CASE(format_esc_question) {
  BOOST_CHECK_EQUAL(formatShortcut({.esc = true, .key = '?'}), "Esc-?");
}

BOOST_AUTO_TEST_CASE(format_ctrl_space) {
  BOOST_CHECK_EQUAL(formatShortcut({.ctrl = true, .key = ' '}), "Ctrl-Space");
}

BOOST_AUTO_TEST_CASE(format_ctrl_h_shows_bksp) {
  // Ctrl-H maps to Ctrl-Backspace in terminals.
  BOOST_CHECK_EQUAL(formatShortcut({.ctrl = true, .key = 'h'}), "Ctrl-Bksp");
}

BOOST_AUTO_TEST_CASE(format_esc_backspace) {
  BOOST_CHECK_EQUAL(formatShortcut({.esc = true, .key = 0x7F}), "Esc-Bksp");
}

// --- descriptionForAction ---

BOOST_AUTO_TEST_CASE(description_save) {
  BOOST_CHECK_EQUAL(descriptionForAction(Action::Save), "Save file");
}

BOOST_AUTO_TEST_CASE(description_find) {
  BOOST_CHECK_EQUAL(descriptionForAction(Action::Find), "Find forward");
}

BOOST_AUTO_TEST_CASE(description_find_backward) {
  BOOST_CHECK_EQUAL(descriptionForAction(Action::FindBackward), "Find backward");
}

BOOST_AUTO_TEST_CASE(description_revert) {
  BOOST_CHECK_EQUAL(descriptionForAction(Action::Revert), "Revert buffer from file");
}

BOOST_AUTO_TEST_CASE(description_help) {
  BOOST_CHECK_EQUAL(descriptionForAction(Action::Help), "Show this help");
}

// --- shortcutFromCtrlChar ---

BOOST_AUTO_TEST_CASE(ctrl_char_nul_is_ctrl_space) {
  auto s = shortcutFromCtrlChar(0);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>(' '));
}

BOOST_AUTO_TEST_CASE(ctrl_char_0x13_is_ctrl_s) {
  // Ctrl-S = 0x13 (ASCII 'S' - 0x40 = 0x13).
  auto s = shortcutFromCtrlChar(0x13);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('s'));
}

BOOST_AUTO_TEST_CASE(ctrl_char_0x06_is_ctrl_f) {
  // Ctrl-F = 0x06.
  auto s = shortcutFromCtrlChar(0x06);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('f'));
}

BOOST_AUTO_TEST_CASE(ctrl_char_0x12_is_ctrl_r) {
  // Ctrl-R = 0x12.
  auto s = shortcutFromCtrlChar(0x12);
  BOOST_CHECK(s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('r'));
  // Ctrl-R is now FindBackward.
  auto action = handleShortcut(s);
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::FindBackward);
}

BOOST_AUTO_TEST_CASE(ctrl_char_roundtrip_finds_action) {
  // Ctrl-S as raw char → shortcut → action should be Save.
  auto s = shortcutFromCtrlChar(0x13);
  auto action = handleShortcut(s);
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Save);
}

// --- shortcutFromEscChar ---

BOOST_AUTO_TEST_CASE(esc_char_question) {
  auto s = shortcutFromEscChar('?');
  BOOST_CHECK(s.esc);
  BOOST_CHECK(!s.ctrl);
  BOOST_CHECK_EQUAL(s.key, static_cast<uint32_t>('?'));
}

BOOST_AUTO_TEST_CASE(esc_char_roundtrip_finds_action) {
  auto s = shortcutFromEscChar('c');
  auto action = handleShortcut(s);
  BOOST_REQUIRE(action.has_value());
  BOOST_CHECK(*action == Action::Exit);
}

// --- All actions have descriptions ---

BOOST_AUTO_TEST_CASE(all_actions_have_descriptions) {
  for (auto a : {Action::Help,
                 Action::Exit,
                 Action::GoToLine,
                 Action::Save,
                 Action::KillBuffer,
                 Action::SwitchBuffer,
                 Action::ToggleMark,
                 Action::Cut,
                 Action::Copy,
                 Action::Paste,
                 Action::PasteCycle,
                 Action::DeleteChunkBefore,
                 Action::Revert,
                 Action::Find,
                 Action::FindBackward,
                 Action::BufferList,
                 Action::PasteRing,
                 Action::ToggleWhitespace,
                 Action::ToggleWrap,
                 Action::Format,
                 Action::SetMode,
                 Action::DirBrowser,
                 Action::PipeReplace,
                 Action::SplitVertical,
                 Action::SplitHorizontal,
                 Action::ClosePanel,
                 Action::FocusPanel1,
                 Action::FocusPanel2,
                 Action::FocusPanel3,
                 Action::FocusPanel4,
                 Action::FocusPanel5,
                 Action::FocusPanel6}) {
    BOOST_CHECK(!descriptionForAction(a).empty());
  }
}

// --- All actions have shortcuts ---

BOOST_AUTO_TEST_CASE(all_actions_have_shortcuts) {
  for (auto a : {Action::Help,
                 Action::Exit,
                 Action::GoToLine,
                 Action::Save,
                 Action::KillBuffer,
                 Action::SwitchBuffer,
                 Action::ToggleMark,
                 Action::Cut,
                 Action::Copy,
                 Action::Paste,
                 Action::PasteCycle,
                 Action::DeleteChunkBefore,
                 Action::Revert,
                 Action::Find,
                 Action::FindBackward,
                 Action::BufferList,
                 Action::PasteRing,
                 Action::ToggleWhitespace,
                 Action::ToggleWrap,
                 Action::Format,
                 Action::SetMode,
                 Action::DirBrowser,
                 Action::PipeReplace,
                 Action::SplitVertical,
                 Action::SplitHorizontal,
                 Action::ClosePanel,
                 Action::FocusPanel1,
                 Action::FocusPanel2,
                 Action::FocusPanel3,
                 Action::FocusPanel4,
                 Action::FocusPanel5,
                 Action::FocusPanel6}) {
    auto s = shortcutForAction(a);
    // Every action should have either ctrl or esc set.
    BOOST_CHECK(s.ctrl || s.esc);
  }
}
