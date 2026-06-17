#pragma once

/**
 * @file mouse.hpp
 * @brief SGR mouse parser: decode `ESC[<…M/m` payloads from the input
 *        loop into structured MouseEvent values.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

/**
 * @brief A mouse event decoded from an `ESC[<b;col;row;M` (or `…m`)
 *        SGR sequence.
 *
 * @c button is the raw SGR button code; its bit layout is documented by
 * the bit-mask constants below. @c press is true for the `M`-terminated
 * variant (button down) and false for `m` (release). @c row and @c col
 * are 1-based screen coordinates as reported by the terminal.
 */
struct MouseEvent {
  std::uint32_t button = 0; /**< Raw SGR button code. */
  std::size_t col = 0;      /**< 1-based screen column. */
  std::size_t row = 0;      /**< 1-based screen row. */
  bool press = true;        /**< True for press (`M`), false for release (`m`). */

  /**
   * @name SGR button-code bit layout
   *
   * bits 0-1: button index (0=left, 1=middle, 2=right);
   * bits 2-4: shift / alt / ctrl modifiers;
   * bit 5: motion (drag) marker; bit 6: wheel-event marker.
   */
  /** @{ */
  static constexpr std::uint32_t kWheelBit = 0x40;
  static constexpr std::uint32_t kMotionBit = 0x20;
  static constexpr std::uint32_t kButtonMask = 0x03;
  /** @} */

  /** @brief True if this is a button-press event. */
  bool isPress() const { return press; }
  /** @brief True if this is a button-release event. */
  bool isRelease() const { return !press; }
  /** @brief True if this is a scroll-wheel event. */
  bool isWheel() const { return (button & kWheelBit) != 0; }
  /** @brief True if this is a wheel-up scroll event. */
  bool isWheelUp() const { return isWheel() && (button & kButtonMask) == 0; }
  /** @brief True if this is a wheel-down scroll event. */
  bool isWheelDown() const { return isWheel() && (button & kButtonMask) == 1; }
  /** @brief True if this event carries the motion-while-held (drag) marker. */
  bool isMotion() const { return (button & kMotionBit) != 0; }
  /** @brief True if the left button is the subject of this event. */
  bool isLeftButton() const { return !isWheel() && (button & kButtonMask) == 0; }
  /** @brief True if the middle button is the subject of this event. */
  bool isMiddleButton() const { return !isWheel() && (button & kButtonMask) == 1; }
};

/**
 * @brief Upper bound on the SGR mouse body the input loop accumulates
 *        before giving up on finding a terminator.
 *
 * A real payload — three decimal integers, separators, and the M/m
 * terminator — is well under 32 bytes; 64 leaves slack for over-helpful
 * terminals without letting a hostile or broken stream grow the buffer
 * unboundedly.
 */
constexpr std::size_t kMaxMouseBody = 64;

/**
 * @brief Collect the bytes of an SGR mouse body.
 *
 * Pulls bytes from @p readByte until an SGR terminator ('M' or 'm') is
 * seen or the body reaches ::kMaxMouseBody. The result (including the
 * terminator when present) is passed to parseSgrMouse(). Templated so
 * callers driving real stdin and callers driving a synthetic queue
 * (tests) share the same collection logic.
 *
 * @tparam ReadByte Callable `bool(char&)` returning true and setting its
 *         out-parameter on success, false on EOF/error.
 * @param readByte The byte source.
 * @return The collected body bytes.
 */
template <typename ReadByte> std::string collectSgrMouseBody(ReadByte readByte) {
  std::string body;
  body.reserve(kMaxMouseBody);
  char ch;
  while (readByte(ch)) {
    body += ch;
    if (ch == 'M' || ch == 'm')
      break;
    if (body.size() >= kMaxMouseBody)
      break;
  }
  return body;
}

/**
 * @brief Parse the SGR mouse payload that follows `ESC[<`.
 *
 * @param tail The payload; should end with the terminating `M` or `m`.
 *        Any extra bytes after the terminator are ignored.
 * @return The decoded MouseEvent, or nullopt on malformed input.
 */
inline std::optional<MouseEvent> parseSgrMouse(std::string_view tail) {
  if (tail.empty())
    return std::nullopt;
  // Find terminator.
  std::size_t end = 0;
  while (end < tail.size() && tail[end] != 'M' && tail[end] != 'm')
    ++end;
  if (end >= tail.size())
    return std::nullopt;

  auto body = tail.substr(0, end);
  // Body must be three semicolon-separated decimal integers.
  std::uint32_t fields[3] = {0, 0, 0};
  int idx = 0;
  bool sawDigit = false;
  for (char c : body) {
    if (c == ';') {
      if (!sawDigit || idx >= 2)
        return std::nullopt;
      ++idx;
      sawDigit = false;
      continue;
    }
    if (c < '0' || c > '9')
      return std::nullopt;
    // Cap at 32 bits. Coordinates over 1e6 are nonsensical anyway.
    if (fields[idx] > 100000000u)
      return std::nullopt;
    fields[idx] = fields[idx] * 10 + static_cast<std::uint32_t>(c - '0');
    sawDigit = true;
  }
  if (idx != 2 || !sawDigit)
    return std::nullopt;

  MouseEvent e;
  e.button = fields[0];
  e.col = fields[1];
  e.row = fields[2];
  e.press = (tail[end] == 'M');
  return e;
}
