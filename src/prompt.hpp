#pragma once

/**
 * @file prompt.hpp
 * @brief The status-line prompt machinery: the Prompt struct that
 *        describes an active prompt, its display cache, and per-label
 *        input history.
 *
 * Prompts are constructed by Screen's various @c openXxxPrompt() helpers
 * and driven generically by Screen::handlePromptInput().
 */

#include "byte_constants.hpp"
#include "utf8.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

/** @brief Hint text drawn after the prompt label+input on the status line. */
struct PromptDisplay {
  std::string rendered;    /**< ANSI-formatted hint (may contain escapes). */
  std::size_t visualWidth; /**< Display columns of @c rendered (excluding escapes). */
};

/**
 * @brief Build a dim-styled hint echoing @p hint for the autocomplete
 *        area of a prompt.
 *
 * Typically used to redisplay the user's previous Find/Regex search.
 * @p hint is escaped via escapeNonPrintable() so a stray control byte
 * (e.g. an ESC that slipped past an input filter) cannot be written
 * verbatim into the modeline.
 *
 * @param hint The text to echo.
 * @return The styled, width-measured display hint.
 */
inline PromptDisplay buildDimHint(std::string const& hint) {
  if (hint.empty())
    return {"", 0};
  auto escaped = escapeNonPrintable(hint);
  auto vw = visualWidth(escaped);
  return {std::string(ansi::Dim) + escaped + std::string(ansi::DimOff), vw};
}

/**
 * @brief A prompt shown on the status line.
 *
 * Constructed by the various @c openXxxPrompt() helpers in Screen and
 * driven generically by Screen::handlePromptInput(). The callbacks let
 * each prompt type customise autocomplete, completion, abort, and special
 * keys without the generic driver knowing the specifics.
 */
struct Prompt {
  /**
   * @brief Result of an autocomplete call: a display hint for a partial
   *        match, or a string that closes the prompt as a unique match.
   */
  using AutocompleteResult = std::variant<PromptDisplay, std::string>;

  std::string label;     /**< Prompt label, e.g. "Go to line: ", "Buffer: ". */
  std::string input;     /**< Current typed text. */
  bool showInput = true; /**< False for confirm prompts (label IS the display). */

  /**
   * @brief Called after every input change; returns a display hint or an
   *        early-exit result value that closes the prompt.
   */
  std::function<AutocompleteResult(std::string const& input)> autocomplete;

  /**
   * @brief Called when the prompt completes (Enter, or autocomplete
   *        returned a result).
   */
  std::function<void(std::string const& value)> onResult;

  /** @brief Called when the user presses Esc. */
  std::function<void()> onAbort;

  /** @brief Optional Tab handler; may modify @c input, returns updated display. */
  std::function<PromptDisplay(std::string& input)> onTab;

  /**
   * @brief Optional Right-arrow handler; may modify @c input, returns
   *        updated display.
   */
  std::function<PromptDisplay(std::string& input)> onRightArrow;

  /**
   * @brief Optional handler for control chars the generic driver does not
   *        consume (e.g. Ctrl-F/Ctrl-R in Find). Returns true if consumed.
   */
  std::function<bool(char key, std::string& input)> onSpecialKey;

  /** @brief Cached display from the last autocomplete call (read by draw()). */
  PromptDisplay display;

  std::optional<std::size_t> historyIdx; /**< Current history index, if browsing. */
  std::string savedInput;                /**< Input stashed before history navigation. */
};

/** @brief Per-label history of completed prompt inputs. */
class PromptHistory {
public:
  /** @brief Maximum entries retained per label. */
  static constexpr std::size_t kMaxPerLabel = 50;

  /**
   * @brief Record a completed input under @p label.
   *
   * Empty values are ignored; an existing duplicate is moved to the front
   * rather than duplicated; the list is capped at ::kMaxPerLabel.
   * @param label Prompt label the value belongs to.
   * @param value The completed input (ownership taken).
   */
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

  /**
   * @brief Most-recent-first history for @p label.
   * @return Pointer to the entry list, or nullptr if there is no history.
   */
  std::vector<std::string> const* get(std::string const& label) const {
    auto it = _entries.find(label);
    if (it == _entries.end() || it->second.empty())
      return nullptr;
    return &it->second;
  }

private:
  std::unordered_map<std::string, std::vector<std::string>> _entries;
};
