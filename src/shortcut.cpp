#include "shortcut.hpp"
#include <algorithm>
#include <array>
#include <unordered_set>

/** @brief One row of the keybinding table: shortcut, action, description. */
struct Binding {
  Shortcut shortcut;
  Action action;
  std::string_view description;
};

/**
 * @brief The single source of truth for all keybindings.
 *
 * Both help display and input dispatch read from this table; the order
 * matches the help-screen layout.
 */
static constexpr auto kBindings = std::array<Binding, 54>{{
    {{.esc = true, .key = '?'}, Action::Help, "Show this help"},
    {{.esc = true, .key = 'c'}, Action::Exit, "Exit"},
    {{.ctrl = true, .key = 'o'}, Action::SwitchBuffer, "Switch to buffer/open file"},
    {{.esc = true, .key = 'b'}, Action::BufferList, "Buffer list"},
    {{.ctrl = true, .key = 's'}, Action::Save, "Save file"},
    {{.ctrl = true, .key = 'k'}, Action::DeleteLine, "Delete current line"},
    {{.cx = true, .key = 'k'}, Action::KillBuffer, "Kill buffer"},
    {{.esc = true, .key = 'r'}, Action::Revert, "Revert buffer from file"},
    {{.ctrl = true, .key = 'u'}, Action::VersionHistory, "Undo / version history"},
    {{.ctrl = true, .key = 'f'}, Action::Find, "Find forward"},
    {{.ctrl = true, .key = 'r'}, Action::FindBackward, "Find backward"},
    {{.esc = true, .key = '%'}, Action::Replace, "Find and replace"},
    {{.esc = true, .key = 's'}, Action::ToggleWhitespace, "Toggle whitespace"},
    {{.esc = true, .key = 'l'}, Action::ToggleWrap, "Toggle line wrap"},
    {{.ctrl = true, .key = ' '}, Action::ToggleMark, "Set/clear mark"},
    {{.ctrl = true, .key = 'w'}, Action::Cut, "Cut region"},
    {{.esc = true, .key = 'w'}, Action::Copy, "Copy region"},
    {{.ctrl = true, .key = 'y'}, Action::Paste, "Paste"},
    {{.esc = true, .key = 'y'}, Action::PasteCycle, "Paste cycle"},
    {{.esc = true, .key = 'p'}, Action::PasteRing, "Paste ring"},
    {{.ctrl = true, .key = 'g'}, Action::GoToLine, "Go to line"},
    {{.ctrl = true, .key = 'h'}, Action::DeleteChunkBefore, "Delete chunk before cursor"},
    {{.esc = true, .key = 0x7F}, Action::DeleteChunkBefore, "Delete chunk before cursor"},
    {{.esc = true, .key = 'f'}, Action::Format, "Format buffer/region"},
    {{.esc = true, .key = ';'}, Action::ToggleComment, "Toggle comment"},
    {{.esc = true, .key = 'U'}, Action::Uppercase, "Uppercase region/word"},
    {{.esc = true, .key = 'L'}, Action::Lowercase, "Lowercase region/word"},
    {{.esc = true, .key = 'C'}, Action::Capitalize, "Capitalize region/word"},
    {{.ctrl = true, .key = 'p'}, Action::PipeReplace, "Pipe region through command"},
    {{.esc = true, .key = 'm'}, Action::SetMode, "Set buffer mode"},
    {{.esc = true, .key = 'd'}, Action::DirBrowser, "Directory browser"},
    {{.cx = true, .key = 'v'}, Action::SplitVertical, "Split panel vertical"},
    {{.cx = true, .key = 'h'}, Action::SplitHorizontal, "Split panel horizontal"},
    {{.cx = true, .key = '.'}, Action::ClosePanel, "Close panel"},
    {{.cx = true, .key = 'o'}, Action::FocusMRUPanel, "Other panel (MRU)"},
    {{.cx = true, .key = '1'}, Action::FocusPanel1, "Focus panel 1"},
    {{.cx = true, .key = '2'}, Action::FocusPanel2, "Focus panel 2"},
    {{.cx = true, .key = '3'}, Action::FocusPanel3, "Focus panel 3"},
    {{.cx = true, .key = '4'}, Action::FocusPanel4, "Focus panel 4"},
    {{.cx = true, .key = '5'}, Action::FocusPanel5, "Focus panel 5"},
    {{.cx = true, .key = '6'}, Action::FocusPanel6, "Focus panel 6"},
    {{.ctrl = true, .key = 'n'}, Action::NextBlock, "Next block (hunk/conflict)"},
    {{.ctrl = true, .key = 'b'}, Action::PrevBlock, "Prev block (hunk/conflict)"},
    {{.ctrl = true, .key = 'l'}, Action::Recenter, "Recenter cursor (mid/top/bot)"},
    {{.cx = true, .key = 'b'}, Action::SetBookmark, "Set bookmark"},
    {{.cx = true, .key = 'j'}, Action::JumpBookmark, "Jump to bookmark"},
    {{.esc = true, .key = '_'}, Action::ToggleCurrentLine, "Highlight current line"},
    {{.esc = true, .key = 'x'}, Action::ExecuteCommand, "Execute command by name"},
    {{.esc = true, .key = 'q'}, Action::Reflow, "Reflow paragraph (80 cols)"},
    {{.esc = true, .key = 'X'}, Action::RegexFind, "Regex find forward"},
    {{.esc = true, .key = 'Y'}, Action::RegexReplace, "Regex find and replace"},
    {{.esc = true, .key = '#'}, Action::ToggleLineNumbers, "Toggle line-number gutter"},
    {{.cx = true, .key = 'm'}, Action::ToggleMouseTracking, "Toggle mouse tracking"},
    {{}, Action::ToggleOverwriteMode, "Toggle insert/overwrite mode"},
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

std::vector<std::pair<std::string_view, Action>> allCommands() {
  std::vector<std::pair<std::string_view, Action>> out;
  std::unordered_set<int> seen;
  for (auto& b : kBindings) {
    if (b.action == Action::ExecuteCommand)
      continue;
    if (!seen.insert(static_cast<int>(b.action)).second)
      continue; // skip duplicates (e.g. DeleteChunkBefore has two bindings)
    out.emplace_back(b.description, b.action);
  }
  std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
  return out;
}

std::string formatShortcut(Shortcut s) {
  // Ctrl-H is Ctrl-Backspace in terminals.
  if (s.ctrl && s.key == 'h')
    return "Ctrl-Bksp";

  std::string result;
  if (s.cx)
    result = "C-x ";
  else if (s.ctrl)
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
