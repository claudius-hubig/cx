#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// SGR mouse event decoded from `ESC[<b;col;row;M` or `...m` sequences.
//
// `button` is the raw SGR button code: bits 0-1 are the button index
// (0=left, 1=middle, 2=right), bit 2 is shift, bit 3 is alt, bit 4 is
// ctrl, bit 5 distinguishes motion-while-held from a fresh press, and
// bit 6 marks wheel events (wheel-up=64, wheel-down=65). `press` is
// true for the `M`-terminated variant (button down) and false for `m`
// (release). `row` and `col` are 1-based screen coordinates as
// reported by the terminal.
struct MouseEvent {
  std::uint32_t button = 0;
  std::size_t col = 0;
  std::size_t row = 0;
  bool press = true;

  // SGR button-code bit layout:
  //   bits 0-1: button index (0=left, 1=middle, 2=right)
  //   bits 2-4: shift / alt / ctrl modifiers
  //   bit 5:    motion (drag) marker
  //   bit 6:    wheel-event marker
  static constexpr std::uint32_t kWheelBit = 0x40;
  static constexpr std::uint32_t kMotionBit = 0x20;
  static constexpr std::uint32_t kButtonMask = 0x03;

  bool isPress() const { return press; }
  bool isRelease() const { return !press; }
  bool isWheel() const { return (button & kWheelBit) != 0; }
  bool isWheelUp() const { return isWheel() && (button & kButtonMask) == 0; }
  bool isWheelDown() const { return isWheel() && (button & kButtonMask) == 1; }
  bool isMotion() const { return (button & kMotionBit) != 0; }
  bool isLeftButton() const { return !isWheel() && (button & kButtonMask) == 0; }
};

// Upper bound on the SGR mouse body the input loop is willing to
// accumulate before giving up on finding a terminator. A real payload
// — three decimal integers plus separators plus the M/m terminator —
// is well under 32 bytes; 64 leaves slack for over-helpful terminals
// without letting a hostile or broken stream grow the buffer
// unboundedly.
constexpr std::size_t kMaxMouseBody = 64;

// Pull bytes from `readByte` until either an SGR terminator ('M' or
// 'm') is seen or the body reaches kMaxMouseBody. Returns the collected
// bytes including the terminator (when present); caller passes the
// result to `parseSgrMouse`. `readByte` returns true and sets its
// out-parameter on success, false on EOF/error. Templated so callers
// driving real stdin and callers driving a synthetic queue (tests)
// share the same collection logic.
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

// Parse the SGR mouse payload that follows `ESC[<`. `tail` should end
// with the terminating `M` or `m`; any extra bytes after the
// terminator are ignored. Returns nullopt on malformed input.
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
