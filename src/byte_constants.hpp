#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Named constants for special byte values used in terminal input handling.
// The same underlying value may appear under different names when it serves
// different semantic roles (e.g. 'C' is both a Ctrl-key offset and the
// ANSI right-arrow final byte).

namespace key {

// --- Control characters (0x00–0x1F) ---
constexpr char Tab = '\t';
constexpr char Enter = '\r';
constexpr char Newline = '\n';
constexpr char Escape = '\x1b';
constexpr char CtrlF = '\x06'; // Find
constexpr char CtrlR = '\x12'; // Reverse find
constexpr char CtrlX = '\x18'; // Prefix for C-x chord

// --- Printable threshold ---
// Characters below this are control characters.
constexpr unsigned char PrintableMin = 0x20;

// --- Special non-printable above 0x7E ---
constexpr char Backspace = '\x7f'; // Terminal sends DEL for Backspace

} // namespace key

namespace ansi {

// --- CSI (Control Sequence Introducer) ---
constexpr char CsiIntro = '['; // Byte after ESC that starts a CSI sequence

// --- Final bytes of CSI cursor/arrow sequences (ESC [ X) ---
constexpr char UpArrow = 'A';
constexpr char DownArrow = 'B';
constexpr char RightArrow = 'C';
constexpr char LeftArrow = 'D';
constexpr char Home = 'H';
constexpr char End = 'F';

// --- Parameter bytes in extended sequences ---
constexpr char ModifiedKey = '1'; // ESC[1;modX — modified arrow/home/end
constexpr char Delete = '3';      // ESC[3~ — Delete key
constexpr char PageUp = '5';      // ESC[5~ — Page Up
constexpr char PageDown = '6';    // ESC[6~ — Page Down
constexpr char Separator = ';';   // Parameter separator in CSI sequences
constexpr char Terminator = '~';  // Final byte for extended key sequences

// --- Modifier codes (the digit after ';' in ESC[1;modX) ---
constexpr char ModCtrl = '5'; // Ctrl modifier

// --- Output escape sequences (CSI SGR and editing) ---
// Declared as string_view so callers using `+=` against std::string get
// the size-aware overload and avoid an internal strlen() per write.
constexpr std::string_view Reset = "\x1b[0m";
constexpr std::string_view Bold = "\x1b[1m";
constexpr std::string_view Dim = "\x1b[2m";
constexpr std::string_view DimOff = "\x1b[22m";
constexpr std::string_view ReverseVideo = "\x1b[7m";
constexpr std::string_view DefaultFg = "\x1b[39m";
constexpr std::string_view EraseLine = "\x1b[K";
constexpr std::string_view ShowCursor = "\x1b[?25h";
constexpr std::string_view CsiPrefix = "\x1b["; // start of CUP / parameterised sequences

/// Return the ANSI CUP sequence to move the cursor to (row, col).
/// Both row and col are 0-based and converted to 1-based for the terminal.
inline std::string cursorTo(std::size_t row, std::size_t col) {
  return "\x1b[" + std::to_string(row + 1) + ";" + std::to_string(col + 1) + "H";
}

} // namespace ansi
