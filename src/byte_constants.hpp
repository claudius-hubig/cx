#pragma once

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

} // namespace ansi
