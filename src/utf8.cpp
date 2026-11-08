#include "utf8.hpp"

#include "byte_constants.hpp"

#include <algorithm>
#include <cassert>
#include <unistd.h>

/**
 * @brief Compute the visual display width of a string.
 *
 * Accounts for multi-byte UTF-8 characters (CJK wide characters return 2)
 * and ANSI escape sequences (treated as zero-width).
 *
 * @param s The string to measure.
 * @return The visual width in terminal columns.
 */
std::size_t visualWidth(std::string_view s) {
  std::size_t w = 0;
  for (std::size_t i = 0; i < s.size();) {
    auto ci = displayCharInfoAt(s.data() + i, s.size() - i);
    w += ci.width;
    i += ci.len;
  }
  return w;
}

/**
 * @brief Replace non-printable bytes in a string with visible escape sequences.
 *
 * Converts control characters (< 0x20, DEL), DEL (0x7F), and invalid
 * UTF-8 lead bytes (>= 0x80 with len==1 from decodeUtf8) to "\\xHH" format.
 * Valid UTF-8 sequences pass through unchanged.
 *
 * The escaped bytes are styled with CtrlColor/DefaultFg for visibility.
 * Adjacent escaped bytes are coalesced into a single styled span.
 *
 * @param s The input string that may contain non-printable bytes.
 * @return A new string with non-printable bytes replaced by visible escapes.
 */
std::string escapeNonPrintable(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  static constexpr char kHex[] = "0123456789abcdef";
  // Wrap each escaped byte in CtrlColor / DefaultFg so it stands out
  // the same way broken bytes do in the buffer text view. Adjacent
  // escaped bytes get coalesced into one styled run to avoid emitting
  // a CSI pair per byte. The closer is DefaultFg (not Reset) so that
  // surrounding state — most importantly ReverseVideo on the modeline
  // / prompt line — survives the styled span; only the foreground
  // colour returns to default. visualWidth() and the buffer renderer
  // both treat CSI sequences as zero-width pass-throughs, so this
  // works for direct-to-terminal callers (modeline, prompts) and
  // for renderer-fed strings (buffer list, dir buffer) alike.
  bool styled = false;
  auto openStyle = [&] {
    if (!styled) {
      out.append(ansi::CtrlColor.data(), ansi::CtrlColor.size());
      styled = true;
    }
  };
  auto closeStyle = [&] {
    if (styled) {
      out.append(ansi::DefaultFg.data(), ansi::DefaultFg.size());
      styled = false;
    }
  };
  auto appendHex = [&](unsigned char u) {
    openStyle();
    out += "\\x";
    out += kHex[u >> 4];
    out += kHex[u & 0xf];
  };
  std::size_t i = 0;
  while (i < s.size()) {
    auto u = static_cast<unsigned char>(s[i]);
    if (u < 0x20 || u == 0x7f) {
      appendHex(u);
      ++i;
    } else if (u >= 0x80) {
      // decodeUtf8 returns len == 1 for any byte that does not start a
      // valid UTF-8 sequence — render those as hex too so broken bytes
      // do not paint as garbage glyphs.
      auto [_, len] = decodeUtf8(s.data() + i, s.size() - i);
      if (len == 1) {
        appendHex(u);
        ++i;
      } else {
        closeStyle();
        out.append(s.data() + i, len);
        i += len;
      }
    } else {
      closeStyle();
      out += s[i];
      ++i;
    }
  }
  closeStyle();
  return out;
}

/**
 * @brief Truncate a string from the front to fit a maximum visual width.
 *
 * If the string's visual width exceeds @p maxWidth, removes characters
 * from the beginning and prepends a horizontal ellipsis ("…", width 1).
 * Otherwise returns the string unchanged.
 *
 * Truncation only occurs on character boundaries (never splits multi-byte
 * UTF-8 sequences or ANSI escape sequences).
 *
 * @param s The string to potentially truncate.
 * @param maxWidth The maximum visual width in terminal columns.
 * @return The truncated string, or original if it fits.
 */
std::string truncateVisualFromFront(std::string_view s, std::size_t maxWidth) {
  if (maxWidth == 0)
    return {};
  std::size_t total = visualWidth(s);
  if (total <= maxWidth)
    return std::string(s);
  // Reserve one visual column for the leading ellipsis.
  std::size_t tailBudget = maxWidth - 1;
  std::size_t i = 0;
  std::size_t dropped = 0;
  while (i < s.size() && total - dropped > tailBudget) {
    auto info = displayCharInfoAt(s.data() + i, s.size() - i);
    dropped += info.width;
    i += info.len;
  }
  // U+2026 HORIZONTAL ELLIPSIS (width 1, 3 bytes UTF-8).
  std::string out = "\xe2\x80\xa6";
  out.append(s.data() + i, s.size() - i);
  return out;
}

/**
 * @brief Read UTF-8 continuation bytes from stdin and append to destination.
 *
 * After a lead byte has already been appended to @p dest, this function
 * reads the remaining continuation bytes (if any) from stdin.
 *
 * If the sequence is malformed (non-continuation byte encountered), the
 * partial sequence (lead + any accepted continuation bytes) is removed from
 * @p dest and the offending byte is discarded.
 *
 * @param lead The lead byte already appended to @p dest.
 * @param dest The destination string to append continuation bytes to.
 */
void readUtf8Tail(unsigned char lead, std::string& dest) {
  appendUtf8Tail(lead, dest, [](char& out) { return read(STDIN_FILENO, &out, 1) == 1; });
}

/**
 * @brief Compute the visual column width of a character range within a line.
 *
 * Sums the display widths of characters in the half-open range [from, to).
 * Stops at the end of the line if to exceeds line.size().
 *
 * @param line The line string.
 * @param from The starting byte offset (inclusive).
 * @param to The ending byte offset (exclusive).
 * @return The visual column width of the range.
 */
std::size_t visualColRange(std::string const& line, std::size_t from, std::size_t to) {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

/**
 * @brief Find the byte offset corresponding to a target visual column.
 *
 * Walks the line from @p from to find the byte offset where the cumulative
 * visual width first exceeds @p goal. Stops at @p to.
 *
 * @param line The line string.
 * @param from The starting byte offset for the search.
 * @param to The maximum byte offset to search (exclusive).
 * @param goal The target visual column.
 * @return The byte offset corresponding to the target, clamped to [from, to].
 */
std::size_t offsetForVisualCol(std::string const& line, std::size_t from, std::size_t to,
                               std::size_t goal) {
  std::size_t pos = from, v = 0;
  std::size_t end = std::min(to, line.size());
  while (pos < end) {
    auto info = charInfoAt(line.data() + pos, line.size() - pos);
    if (v + info.width > goal)
      break;
    v += info.width;
    pos += info.len;
  }
  return std::min(pos, end);
}

/**
 * @brief Find visual row boundaries around a byte offset in a wrapped line.
 *
 * Scans the line to determine which visual row contains @p byteOff and
 * returns the byte offsets for the previous, current, and next visual rows.
 *
 * @param line The line string (may be longer than one visual row when wrapped).
 * @param byteOff The byte offset of interest.
 * @param cols The terminal width in columns for wrapping.
 * @return VisualRowInfo containing prevRowStart, curRowStart, nextRowStart byte offsets.
 */
VisualRowInfo findVisualRow(std::string const& line, std::size_t byteOff, std::size_t cols) {
  std::size_t prevStart = 0, curStart = 0;
  std::size_t col = 0;

  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col > 0 && col + info.width > cols) {
      if (byteOff < i)
        return {prevStart, curStart, i};
      prevStart = curStart;
      curStart = i;
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      std::size_t next = i + info.len;
      if (byteOff < next)
        return {prevStart, curStart, next};
      prevStart = curStart;
      curStart = next;
      col = 0;
    }
    i += info.len;
  }
  return {prevStart, curStart, line.size()};
}

/**
 * @brief Find the byte offset of the last visual row's start in a wrapped line.
 *
 * Scans the line to find where the last visual row begins when the line
 * is wrapped at @p cols columns.
 *
 * @param line The line string.
 * @param cols The terminal width in columns for wrapping.
 * @return The byte offset where the last visual row starts.
 */
std::size_t lastVisualRowStart(std::string const& line, std::size_t cols) {
  std::size_t rowStart = 0, col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col > 0 && col + info.width > cols) {
      rowStart = i;
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      rowStart = i + info.len;
      col = 0;
    }
    i += info.len;
  }
  return rowStart;
}

/**
 * @brief Find visual row boundaries using precomputed row start offsets.
 *
 * Uses binary search on @p rowStarts (computed by computeRowBounds) to
 * efficiently locate the visual row containing @p byteOff.
 *
 * @param rowStarts Precomputed byte offsets for each visual row (must not be empty).
 * @param byteOff The byte offset of interest.
 * @param lineSize The total size of the line in bytes.
 * @return VisualRowInfo containing prevRowStart, curRowStart, nextRowStart byte offsets.
 */
VisualRowInfo findVisualRowCached(std::vector<std::size_t> const& rowStarts, std::size_t byteOff,
                                  std::size_t lineSize) {
  // computeRowBounds always pushes 0 first, so callers see a non-empty
  // vector. Make the precondition explicit so any future regression
  // (e.g. a reset path that forgets the seed) fails loudly rather than
  // underflowing the rowIdx subtraction below.
  assert(!rowStarts.empty());
  auto it = std::upper_bound(rowStarts.begin(), rowStarts.end(), byteOff);
  std::size_t rowIdx = static_cast<std::size_t>(it - rowStarts.begin()) - 1;

  std::size_t curStart = rowStarts[rowIdx];
  std::size_t prevStart = rowIdx > 0 ? rowStarts[rowIdx - 1] : curStart;
  std::size_t nextStart = rowIdx + 1 < rowStarts.size() ? rowStarts[rowIdx + 1] : lineSize;

  return {prevStart, curStart, nextStart};
}

/**
 * @brief Compute byte offsets for each visual row in a wrapped line.
 *
 * Returns a vector where each entry is the byte offset at which a visual row
 * begins. The first entry is always 0. Used for efficient visual row lookup
 * in wrapped lines.
 *
 * @param line The line string.
 * @param cols The terminal width in columns for wrapping.
 * @return Vector of byte offsets marking the start of each visual row.
 */
std::vector<std::size_t> computeRowBounds(std::string const& line, std::size_t cols) {
  std::vector<std::size_t> rowStarts;
  rowStarts.push_back(0);

  std::size_t col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col > 0 && col + info.width > cols) {
      rowStarts.push_back(i);
      col = 0;
    }
    col += info.width;
    if (col >= cols) {
      rowStarts.push_back(i + info.len);
      col = 0;
    }
    i += info.len;
  }
  return rowStarts;
}

/**
 * @brief Read a complete UTF-8 code point from stdin.
 *
 * For ASCII bytes (< 0x80), returns the byte directly.
 * For multi-byte sequences, reads continuation bytes from stdin and decodes.
 * On read error or EOF mid-sequence, returns the lead byte as a raw value.
 *
 * @param lead The first byte of the UTF-8 sequence (already read from stdin).
 * @return The decoded Unicode code point, or the raw lead byte on error.
 */
uint32_t readUtf8Codepoint(unsigned char lead) {
  // ASCII / invalid leads fall straight through: utf8SeqLen returns 1,
  // no continuation bytes are read, and decodeUtf8 yields {lead, 1}.
  char seq[4];
  seq[0] = static_cast<char>(lead);
  std::size_t expected = utf8SeqLen(lead);
  std::size_t got = 1;
  for (std::size_t j = 1; j < expected; ++j) {
    char next;
    if (read(STDIN_FILENO, &next, 1) != 1)
      break;
    seq[got++] = next;
  }
  auto [cp, len] = decodeUtf8(seq, got);
  return cp;
}
