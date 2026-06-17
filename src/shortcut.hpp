#pragma once

/**
 * @file shortcut.hpp
 * @brief The single source of truth for editor keybindings: the Action
 *        enum, the Shortcut struct, and lookup/formatting helpers.
 *
 * A single @c constexpr binding table (in shortcut.cpp) maps Shortcut
 * values to Actions. Both input dispatch and the help display read from
 * it, so the two can never drift.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief One value per editor command.
 *
 * Each Action is bound to a Shortcut in the binding table and carries a
 * human-readable description (see descriptionForAction). Screen's
 * dispatchAction() executes the effect of any Action.
 */
enum class Action {
  Help,
  Exit,
  GoToLine,
  Save,
  KillBuffer,
  DeleteLine,
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
  Replace,
  BufferList,
  PasteRing,
  ToggleWhitespace,
  ToggleWrap,
  Format,
  ToggleComment,
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
  NextBlock,
  PrevBlock,
  Recenter,
  ToggleCurrentLine,
  ExecuteCommand,
  Reflow,
  RegexFind,
  RegexReplace,
  ToggleLineNumbers,
  ToggleMouseTracking,
};

/** @brief A key combination identifying a binding. */
struct Shortcut {
  bool esc = false;  /**< Reached via an Esc-prefix chord. */
  bool ctrl = false; /**< Ctrl is held. */
  bool cx = false;   /**< Ctrl-x prefix (two-key sequence). */
  uint32_t key = 0;  /**< Lowercase letter, ' ' for space, 0x7F for backspace. */
  bool operator==(Shortcut const&) const = default;
};

/**
 * @brief Forward lookup of a Shortcut to its bound Action.
 * @return The Action, or nullopt if the shortcut is unrecognised.
 */
std::optional<Action> handleShortcut(Shortcut s);

/**
 * @brief Reverse lookup of an Action's primary Shortcut.
 * @return The first matching shortcut in the binding table.
 */
Shortcut shortcutForAction(Action a);

/**
 * @brief Format a Shortcut as a human-readable label.
 * @return e.g. "Ctrl-s", "Esc-?", "Ctrl-Bksp".
 */
std::string formatShortcut(Shortcut s);

/** @brief Short description string for an action (e.g. "Save file"). */
std::string_view descriptionForAction(Action a);

/**
 * @brief All unique (description, Action) pairs, sorted alphabetically by
 *        description.
 *
 * Used by Esc-x execute-by-name to populate completions. @c ExecuteCommand
 * itself is excluded so the prompt cannot recurse.
 */
std::vector<std::pair<std::string_view, Action>> allCommands();

/** @brief Convert a raw control char (0x00-0x1F) to a Shortcut. */
inline Shortcut shortcutFromCtrlChar(char c) {
  if (c == 0)
    return {.ctrl = true, .key = ' '};
  return {.ctrl = true, .key = static_cast<uint32_t>(c) | 0x60u};
}

/** @brief Convert a byte received after ESC to a Shortcut. */
inline Shortcut shortcutFromEscChar(char c) {
  return {.esc = true, .key = static_cast<uint32_t>(static_cast<unsigned char>(c))};
}

/** @brief Convert a byte received after the Ctrl-x prefix to a Shortcut. */
inline Shortcut shortcutFromCxChar(char c) {
  return {.cx = true, .key = static_cast<uint32_t>(static_cast<unsigned char>(c))};
}
