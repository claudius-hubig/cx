#include "shortcut.hpp"
#include <array>

struct Binding {
  Shortcut shortcut;
  Action action;
  std::string_view description;
};

// The single source of truth for all keybindings. Help display and input
// dispatch both read from this table. Order matches the help screen layout.
static constexpr auto kBindings = std::array<Binding, 18>{{
    {{.esc = true, .key = '?'}, Action::Help, "Show this help"},
    {{.esc = true, .key = 'c'}, Action::Exit, "Exit"},
    {{.ctrl = true, .key = 'o'}, Action::SwitchBuffer, "Switch to buffer/open file"},
    {{.esc = true, .key = 'b'}, Action::BufferList, "Buffer list"},
    {{.ctrl = true, .key = 's'}, Action::Save, "Save file"},
    {{.ctrl = true, .key = 'k'}, Action::KillBuffer, "Kill buffer"},
    {{.ctrl = true, .key = 'r'}, Action::Revert, "Revert buffer from file"},
    {{.esc = true, .key = 's'}, Action::ToggleWhitespace, "Toggle whitespace"},
    {{.esc = true, .key = 'l'}, Action::ToggleWrap, "Toggle line wrap"},
    {{.ctrl = true, .key = ' '}, Action::ToggleMark, "Set/clear mark"},
    {{.ctrl = true, .key = 'w'}, Action::Cut, "Cut region"},
    {{.esc = true, .key = 'w'}, Action::Copy, "Copy region"},
    {{.ctrl = true, .key = 'y'}, Action::Paste, "Paste"},
    {{.esc = true, .key = 'y'}, Action::PasteCycle, "Paste cycle"},
    {{.esc = true, .key = 'r'}, Action::PasteRing, "Paste ring"},
    {{.ctrl = true, .key = 'g'}, Action::GoToLine, "Go to line"},
    {{.ctrl = true, .key = 'h'}, Action::DeleteChunkBefore, "Delete chunk before cursor"},
    {{.esc = true, .key = 0x7F}, Action::DeleteChunkBefore, "Delete chunk before cursor"},
}};

std::optional<Action> handleShortcut(Shortcut s) {
  for (auto& b : kBindings)
    if (b.shortcut == s)
      return b.action;
  return std::nullopt;
}

Shortcut shortcutForAction(Action a) {
  for (auto& b : kBindings)
    if (b.action == a)
      return b.shortcut;
  return {};
}

std::string_view descriptionForAction(Action a) {
  for (auto& b : kBindings)
    if (b.action == a)
      return b.description;
  return {};
}

std::string formatShortcut(Shortcut s) {
  // Ctrl-H is Ctrl-Backspace in terminals.
  if (s.ctrl && s.key == 'h')
    return "Ctrl-Bksp";

  std::string result;
  if (s.ctrl)
    result = "Ctrl-";
  else if (s.esc)
    result = "Esc-";

  if (s.key == ' ')
    result += "Space";
  else if (s.key == 0x7F)
    result += "Bksp";
  else
    result += static_cast<char>(s.key);

  return result;
}
