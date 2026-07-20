#pragma once

/**
 * @file byte_constants.hpp
 * @brief Named constants for terminal input bytes and ANSI output
 *        sequences.
 *
 * The @c key namespace names raw bytes that arrive on stdin; the @c ansi
 * namespace names the escape sequences the editor writes to the terminal.
 * The same underlying value may appear under different names when it
 * serves different semantic roles (e.g. 'C' is both a Ctrl-key offset and
 * the ANSI right-arrow final byte).
 */

#include <array>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

/** @brief Named byte values used when interpreting terminal input. */
namespace key {

/** @name Control characters (0x00–0x1F) */
/** @{ */
constexpr char Tab = '\t';
constexpr char Enter = '\r';
constexpr char Newline = '\n';
constexpr char Escape = '\x1b';
constexpr char CtrlF = '\x06'; /**< Find */
constexpr char CtrlR = '\x12'; /**< Reverse find */
constexpr char CtrlX = '\x18'; /**< Prefix for the C-x chord */
/** @} */

/** @brief Bytes below this threshold are control characters. */
constexpr unsigned char PrintableMin = 0x20;

/** @brief Byte the terminal sends for the Backspace key (DEL). */
constexpr char Backspace = '\x7f';

} // namespace key

/** @brief Named ANSI escape sequences and CSI bytes used for output. */
namespace ansi {

/** @brief Byte after ESC that starts a CSI (Control Sequence Introducer). */
constexpr char CsiIntro = '[';

/** @name Final bytes of CSI cursor/arrow sequences (ESC [ X) */
/** @{ */
constexpr char UpArrow = 'A';
constexpr char DownArrow = 'B';
constexpr char RightArrow = 'C';
constexpr char LeftArrow = 'D';
constexpr char Home = 'H';
constexpr char End = 'F';
/** @} */

/** @name Parameter bytes in extended key sequences */
/** @{ */
constexpr char ModifiedKey = '1'; /**< ESC[1;modX — modified arrow/home/end */
constexpr char Insert = '2';      /**< ESC[2~ — Insert key */
constexpr char Delete = '3';      /**< ESC[3~ — Delete key */
constexpr char PageUp = '5';      /**< ESC[5~ — Page Up */
constexpr char PageDown = '6';    /**< ESC[6~ — Page Down */
constexpr char Separator = ';';   /**< Parameter separator in CSI sequences */
constexpr char Terminator = '~';  /**< Final byte for extended key sequences */
/** @} */

/** @brief Modifier code (digit after ';' in ESC[1;modX) for the Ctrl key. */
constexpr char ModCtrl = '5';

/**
 * @brief Modifier code for Shift+Ctrl (ESC[1;6X).
 *
 * Distinct from ModCtrl at the byte level, so there is no conflict. Some
 * terminal and tmux configurations swallow these sequences before they
 * reach the application, which is why the actions they drive are also
 * reachable by name through Esc-x.
 */
constexpr char ModCtrlShift = '6';

/**
 * @name Output escape sequences (CSI SGR and editing)
 *
 * Declared as @c string_view so callers using @c += against a
 * std::string get the size-aware overload and avoid an internal
 * strlen() per write.
 */
/** @{ */
constexpr std::string_view Reset = "\x1b[0m";
constexpr std::string_view Bold = "\x1b[1m";
constexpr std::string_view Dim = "\x1b[2m";
constexpr std::string_view DimOff = "\x1b[22m";
constexpr std::string_view ReverseVideo = "\x1b[7m";
constexpr std::string_view DefaultFg = "\x1b[39m";
/**
 * @brief Foreground used by Buffer::render for escaped control bytes and
 *        broken UTF-8. Shared so non-renderer surfaces
 *        (escapeNonPrintable, prompts) can match the styling.
 */
constexpr std::string_view CtrlColor = "\x1b[38;2;204;120;50m";
constexpr std::string_view EraseLine = "\x1b[K";
constexpr std::string_view ShowCursor = "\x1b[?25h";
/** @brief DECSCUSR steady block cursor — used to signal insert mode. */
constexpr std::string_view CursorBlock = "\x1b[2 q";
/** @brief DECSCUSR steady bar cursor — used to signal overwrite mode. */
constexpr std::string_view CursorBar = "\x1b[6 q";
constexpr std::string_view CsiPrefix = "\x1b["; /**< Start of CUP / parameterised sequences */
/** @} */

/**
 * @brief Build the ANSI CUP sequence that moves the cursor to (row, col).
 * @param row 0-based row, converted to the terminal's 1-based coordinate.
 * @param col 0-based column, converted to the terminal's 1-based coordinate.
 * @return The escape sequence as a freshly allocated string.
 */
inline std::string cursorTo(std::size_t row, std::size_t col) {
  return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
}

/**
 * @brief Append the ANSI CUP sequence to @p out without an intermediate
 *        allocation.
 *
 * Buffer::render emits one cursorTo per visual row per frame; the
 * no-alloc variant saves N small heap allocations per draw on a tall
 * terminal.
 *
 * @param out Destination string to append to.
 * @param row 0-based row, converted to 1-based.
 * @param col 0-based column, converted to 1-based.
 */
inline void appendCursorTo(std::string& out, std::size_t row, std::size_t col) {
  out += "\x1b[";
  std::array<char, 32> buf;
  auto end = std::to_chars(buf.data(), buf.data() + buf.size(), row + 1).ptr;
  out.append(buf.data(), end);
  out += ';';
  end = std::to_chars(buf.data(), buf.data() + buf.size(), col + 1).ptr;
  out.append(buf.data(), end);
  out += 'H';
}

} // namespace ansi
