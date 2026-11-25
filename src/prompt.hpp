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

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
/**
 * @brief Prompt text as shown on the modeline: escapeNonPrintable(), with a
 *        newline shown as `\n` and a tab as `\t`, in the colour of the
 *        other escapes, so a multi-line find pattern reads as one would
 *        write it.
 */
inline std::string promptText(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    auto j = s.find_first_of("\n\t", i);
    out += escapeNonPrintable(s.substr(i, j == std::string_view::npos ? j : j - i));
    if (j == std::string_view::npos)
      break;
    out += ansi::CtrlColor;
    out += s[j] == '\n' ? "\\n" : "\\t";
    out += ansi::DefaultFg;
    i = j + 1;
  }
  return out;
}

inline PromptDisplay buildDimHint(std::string const& hint) {
  if (hint.empty())
    return {"", 0};
  auto escaped = promptText(hint);
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
   * @brief The input may hold newlines and tabs: a paste keeps all its
   *        lines (other prompts take the first), and Tab inserts a tab
   *        (if there is no @c onTab). For the find and replace prompts.
   */
  bool multiLine = false;
  /** @brief Ctrl-q was pressed: the next key goes into @c input as it is
   *         (Enter as a newline). */
  bool quoteNext = false;
  /** @brief Enter on an empty input calls @c onResult with "" instead of
   *         closing the prompt. For inputs where empty means something, such
   *         as a replacement that deletes each match. */
  bool acceptEmpty = false;

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

  /**
   * @brief Byte offset of the edit point within @c input.
   *
   * Always on a UTF-8 code-point boundary. Values past the end (including
   * the default @c npos) mean "end of input", so a prompt constructed with
   * a pre-filled @c input starts with the cursor after it. Read it through
   * cursorPos(), never directly.
   */
  std::size_t cursor = std::string::npos;

  /**
   * @brief Selection anchor (byte offset into @c input), if set.
   *
   * The region spans the mark and the cursor. Like @c cursor it sits on a
   * code-point boundary and is clamped to @c input.size() when read; the
   * editing operations below keep it in place across inserts and deletes,
   * as BufferView does for the buffer mark.
   */
  std::optional<std::size_t> mark;

  /** @brief The span [start, end) of @c input inserted by the last paste. */
  struct Paste {
    std::size_t start;
    std::size_t end;
    std::size_t ringIdx; /**< Paste-ring index of the pasted entry. */
  };
  /** @brief Set by a paste so an immediately following paste-cycle can replace it. */
  std::optional<Paste> lastPaste;

  /** @brief The edit point, clamped to @c input.size(). */
  std::size_t cursorPos() const { return std::min(cursor, input.size()); }
  bool cursorAtEnd() const { return cursorPos() == input.size(); }

  /** @brief The selected span [first, second), or nullopt without a mark. */
  std::optional<std::pair<std::size_t, std::size_t>> region() const {
    if (!mark)
      return std::nullopt;
    auto m = std::min(*mark, input.size());
    return std::pair{std::min(m, cursorPos()), std::max(m, cursorPos())};
  }

  /**
   * @brief Note that a callback replaced @c input wholesale (completion,
   *        history): the cursor goes to the end and the mark is dropped.
   */
  void inputReplaced() {
    moveEnd();
    mark.reset();
  }

  /**
   * @name Line editing at the cursor
   *
   * Word boundaries are whitespace, matching BufferView's word movement
   * and chunk deletion. The delete operations return true if @c input
   * changed.
   */
  /** @{ */
  void insert(std::string_view text) {
    auto pos = cursorPos();
    input.insert(pos, text);
    if (mark && *mark > pos)
      *mark += text.size();
    cursor = pos + text.size();
  }
  /**
   * @brief Erase [@p from, @p to) and leave the cursor at @p from.
   * @pre @p from <= @p to <= input.size(), both on code-point boundaries.
   */
  void erase(std::size_t from, std::size_t to) {
    input.erase(from, to - from);
    if (mark && *mark > from)
      *mark = *mark >= to ? *mark - (to - from) : from;
    cursor = from;
  }
  bool toggleMark() {
    if (mark)
      mark.reset();
    else
      mark = cursorPos();
    return mark.has_value();
  }
  /** @brief Erase the region and drop the mark. */
  bool deleteRegion() {
    auto r = region();
    mark.reset();
    if (!r || r->first == r->second)
      return false;
    erase(r->first, r->second);
    return true;
  }
  bool deleteBefore() {
    auto pos = cursorPos();
    if (pos == 0)
      return false;
    erase(prevCharStart(pos), pos);
    return true;
  }
  bool deleteAt() {
    auto pos = cursorPos();
    if (pos == input.size())
      return false;
    erase(pos, nextCharStart(pos));
    return true;
  }
  /** @brief Delete the run of blanks or non-blanks before the cursor. */
  bool deleteChunkBefore() {
    auto pos = cursorPos();
    if (pos == 0)
      return false;
    bool blank = isWhitespace(input[prevCharStart(pos)]);
    auto from = pos;
    while (from > 0 && isWhitespace(input[prevCharStart(from)]) == blank)
      from = prevCharStart(from);
    erase(from, pos);
    return true;
  }
  /** @brief Delete the run of blanks or non-blanks at the cursor. */
  bool deleteChunkAt() {
    auto pos = cursorPos();
    if (pos == input.size())
      return false;
    bool blank = isWhitespace(input[pos]);
    auto to = pos;
    while (to < input.size() && isWhitespace(input[to]) == blank)
      to = nextCharStart(to);
    erase(pos, to);
    return true;
  }
  void moveLeft() { cursor = cursorPos() == 0 ? 0 : prevCharStart(cursorPos()); }
  void moveRight() { cursor = cursorAtEnd() ? input.size() : nextCharStart(cursorPos()); }
  /** @brief Move to the first byte of the word before the cursor. */
  void moveWordLeft() {
    auto pos = cursorPos();
    while (pos > 0 && isWhitespace(input[prevCharStart(pos)]))
      pos = prevCharStart(pos);
    while (pos > 0 && !isWhitespace(input[prevCharStart(pos)]))
      pos = prevCharStart(pos);
    cursor = pos;
  }
  /** @brief Move past the end of the word at or after the cursor. */
  void moveWordRight() {
    auto pos = cursorPos();
    while (pos < input.size() && isWhitespace(input[pos]))
      pos = nextCharStart(pos);
    while (pos < input.size() && !isWhitespace(input[pos]))
      pos = nextCharStart(pos);
    cursor = pos;
  }
  void moveHome() { cursor = 0; }
  void moveEnd() { cursor = input.size(); }
  /** @} */

private:
  /** @brief Previous character start (a character with its combining marks).
   *  @pre 0 < @p pos <= input.size(). */
  std::size_t prevCharStart(std::size_t pos) const { return prevClusterStart(input, pos); }
  /** @brief Next character start. @pre @p pos < input.size(). */
  std::size_t nextCharStart(std::size_t pos) const { return nextClusterStart(input, pos); }
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
