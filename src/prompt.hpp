#pragma once

#include "byte_constants.hpp"
#include "utf8.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Hint text to append after the prompt label+input on the status line.
struct PromptDisplay {
  std::string rendered;    // ANSI-formatted hint (may contain escape sequences)
  std::size_t visualWidth; // display columns of `rendered` (excluding escapes)
};

// Build a dim-styled hint echoing `hint` (typically the user's previous
// Find/Regex search) for redisplay in the autocomplete area of a prompt.
// `hint` is escaped via escapeNonPrintable so a stray control byte
// (e.g. an ESC that slipped past a prior input filter) cannot be
// written verbatim into the modeline.
inline PromptDisplay buildDimHint(std::string const& hint) {
  if (hint.empty())
    return {"", 0};
  auto escaped = escapeNonPrintable(hint);
  auto vw = visualWidth(escaped);
  return {std::string(ansi::Dim) + escaped + std::string(ansi::DimOff), vw};
}

// A prompt shown on the status line. Constructed by the various openXxxPrompt()
// helpers; driven generically by handlePromptInput().
//
// Autocomplete returns either a display hint (partial match) or a result string
// (unique match that should close the prompt immediately).
struct Prompt {
  using AutocompleteResult = std::variant<PromptDisplay, std::string>;

  std::string label;     // e.g. "Go to line: ", "Buffer: "
  std::string input;     // current typed text
  bool showInput = true; // false for confirm prompts (label IS the display)

  // Called after every input change. Returns a display hint or an early-exit
  // result value that closes the prompt.
  std::function<AutocompleteResult(std::string const& input)> autocomplete;

  // Called when the prompt completes (Enter, or autocomplete returned a result).
  std::function<void(std::string const& value)> onResult;

  // Called when the user presses Esc.
  std::function<void()> onAbort;

  // Optional: Tab key handler. May modify input. Returns updated display.
  std::function<PromptDisplay(std::string& input)> onTab;

  // Optional: Right-arrow handler. May modify input. Returns updated display.
  std::function<PromptDisplay(std::string& input)> onRightArrow;

  // Optional: handler for control chars not consumed by the generic handler
  // (e.g. Ctrl-F/Ctrl-R in Find). Returns true if consumed.
  std::function<bool(char key, std::string& input)> onSpecialKey;

  // Cached display from last autocomplete call (read by draw()).
  PromptDisplay display;

  // History navigation state.
  std::optional<std::size_t> historyIdx;
  std::string savedInput;
};

// Per-label history of completed prompt inputs.
class PromptHistory {
public:
  static constexpr std::size_t kMaxPerLabel = 50;

  void push(std::string const& label, std::string value) {
    if (value.empty())
      return;
    auto& vec = _entries[label];
    // Remove duplicate if already present.
    for (auto it = vec.begin(); it != vec.end(); ++it) {
      if (*it == value) {
        vec.erase(it);
        break;
      }
    }
    vec.insert(vec.begin(), std::move(value));
    if (vec.size() > kMaxPerLabel)
      vec.pop_back();
  }

  // Returns nullptr if no history for this label.
  std::vector<std::string> const* get(std::string const& label) const {
    auto it = _entries.find(label);
    if (it == _entries.end() || it->second.empty())
      return nullptr;
    return &it->second;
  }

private:
  std::unordered_map<std::string, std::vector<std::string>> _entries;
};
