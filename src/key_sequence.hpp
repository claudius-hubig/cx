#pragma once

/**
 * @file key_sequence.hpp
 * @brief Decode the escape sequences terminals send for cursor and editing
 *        keys into KeyPress values.
 *
 * Pure functions over the bytes of an already-read sequence, so tests can
 * exercise them without a terminal. Reading the bytes is the input loop's
 * job: it reads a sequence to its final byte before decoding it, so the
 * tail of a sequence cx has no use for (a function key, say) is consumed
 * with it instead of being typed into the buffer.
 */

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

/** @brief The keys cx distinguishes among those that arrive as escape sequences. */
enum class Key { Up, Down, Right, Left, Home, End, PageUp, PageDown, Insert, Delete };

/** @brief A key with the modifiers that were held. */
struct KeyPress {
  Key key;
  bool shift = false;
  bool alt = false;
  bool ctrl = false;

  /** @brief No modifier held. */
  bool plain() const { return !shift && !alt && !ctrl; }
  /** @brief Ctrl and nothing else. */
  bool ctrlOnly() const { return ctrl && !shift && !alt; }
  /** @brief Ctrl+Shift and nothing else. */
  bool ctrlShiftOnly() const { return ctrl && shift && !alt; }

  bool operator==(KeyPress const&) const = default;
};

/** @brief True for the byte that ends a CSI sequence (0x40..0x7E). */
inline constexpr bool isCsiFinal(char c) { return c >= 0x40 && c <= 0x7E; }

/**
 * @brief Decode a CSI key sequence, `ESC [ params final`.
 *
 * Two shapes are in use (xterm, and the VT220 style that tmux, screen and
 * the Linux console still send for some keys):
 * - `ESC [ A` / `ESC [ 1 ; m A` — a letter names the key: A/B/C/D arrows,
 *   H Home, F End;
 * - `ESC [ n ~` / `ESC [ n ; m ~` — a number names it: 1 or 7 Home, 2
 *   Insert, 3 Delete, 4 or 8 End, 5 Page Up, 6 Page Down.
 *
 * The modifier @c m is 1 + a bit mask (1 Shift, 2 Alt, 4 Ctrl).
 *
 * @param params The parameter bytes between `ESC [` and the final byte.
 * @param final The final byte.
 * @return The key, or nullopt for a sequence that is not one of these keys
 *         (function keys, mouse and focus reports, ...).
 */
inline std::optional<KeyPress> decodeCsiKey(std::string_view params, char final) {
  // At most two decimal fields separated by ';'; an absent field is 1.
  auto field = [](std::string_view s) -> std::optional<unsigned> {
    if (s.empty())
      return 1;
    unsigned value = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size())
      return std::nullopt;
    return value;
  };
  auto semi = params.find(';');
  auto number = field(params.substr(0, semi));
  auto modifier = semi == std::string_view::npos ? 1u : field(params.substr(semi + 1));
  if (!number || !modifier || *modifier < 1 || *modifier > 8)
    return std::nullopt;

  std::optional<Key> key;
  if (final == '~') {
    switch (*number) {
    case 1:
    case 7:
      key = Key::Home;
      break;
    case 2:
      key = Key::Insert;
      break;
    case 3:
      key = Key::Delete;
      break;
    case 4:
    case 8:
      key = Key::End;
      break;
    case 5:
      key = Key::PageUp;
      break;
    case 6:
      key = Key::PageDown;
      break;
    }
  } else if (*number == 1) {
    switch (final) {
    case 'A':
      key = Key::Up;
      break;
    case 'B':
      key = Key::Down;
      break;
    case 'C':
      key = Key::Right;
      break;
    case 'D':
      key = Key::Left;
      break;
    case 'H':
      key = Key::Home;
      break;
    case 'F':
      key = Key::End;
      break;
    }
  }
  if (!key)
    return std::nullopt;
  unsigned bits = *modifier - 1;
  return KeyPress{*key, (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0};
}

/**
 * @brief Decode the final byte of an SS3 sequence, `ESC O final`, which
 *        terminals in application-cursor mode send for the unmodified
 *        arrows and Home/End.
 * @return The key, or nullopt for any other final byte (F1..F4 are
 *         `ESC O P` .. `ESC O S`).
 */
inline std::optional<KeyPress> decodeSs3Key(char final) {
  return final == 'A' || final == 'B' || final == 'C' || final == 'D' || final == 'H' ||
                 final == 'F'
             ? decodeCsiKey({}, final)
             : std::nullopt;
}
