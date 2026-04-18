#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class Action {
  Help,
  Exit,
  GoToLine,
  Save,
  KillBuffer,
  SwitchBuffer,
  ToggleMark,
  Cut,
  Copy,
  Paste,
  PasteCycle,
  DeleteChunkBefore,
  Revert,
  VersionHistory,
  Find,
  FindBackward,
  BufferList,
  PasteRing,
  ToggleWhitespace,
  ToggleWrap,
  Format,
  SetMode,
  DirBrowser,
  PipeReplace,
  SplitVertical,
  SplitHorizontal,
  ClosePanel,
  FocusPanel1,
  FocusPanel2,
  FocusPanel3,
  FocusPanel4,
  FocusPanel5,
  FocusPanel6,
  FocusMRUPanel,
};

struct Shortcut {
  bool esc = false;
  bool ctrl = false;
  bool cx = false;  // Ctrl-x prefix (two-key sequence)
  uint32_t key = 0; // lowercase letter, ' ' for space, 0x7F for backspace
  bool operator==(Shortcut const&) const = default;
};

// Forward lookup: Shortcut -> Action (nullopt if unrecognised).
std::optional<Action> handleShortcut(Shortcut s);

// Reverse lookup: Action -> its primary shortcut (first match in table).
Shortcut shortcutForAction(Action a);

// Human-readable label: "Ctrl-s", "Esc-?", "Ctrl-Bksp", etc.
std::string formatShortcut(Shortcut s);

// Description string for an action (e.g. "Save file").
std::string_view descriptionForAction(Action a);

// Convert raw control char (0x00-0x1F) to a Shortcut.
inline Shortcut shortcutFromCtrlChar(char c) {
  if (c == 0)
    return {.ctrl = true, .key = ' '};
  return {.ctrl = true, .key = static_cast<uint32_t>(c) | 0x60u};
}

// Convert byte received after ESC to a Shortcut.
inline Shortcut shortcutFromEscChar(char c) {
  return {.esc = true, .key = static_cast<uint32_t>(static_cast<unsigned char>(c))};
}

// Convert byte received after Ctrl-x to a Shortcut.
inline Shortcut shortcutFromCxChar(char c) {
  return {.cx = true, .key = static_cast<uint32_t>(static_cast<unsigned char>(c))};
}
