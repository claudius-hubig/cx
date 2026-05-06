#include "utf8.hpp"

#include "byte_constants.hpp"

#include <algorithm>
#include <cassert>
#include <unistd.h>

std::size_t visualWidth(std::string_view s) {
  std::size_t w = 0;
  for (std::size_t i = 0; i < s.size();) {
    auto ci = charInfoAt(s.data() + i, s.size() - i);
    w += ci.width;
    i += ci.len;
  }
  return w;
}

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

std::string truncateVisualFromFront(std::string_view s, std::size_t maxWidth) {
  if (maxWidth == 0)
    return {};
  std::size_t total = visualWidth(s);
  if (total <= maxWidth)
    return std::string(s);
  // Reserve one visual column for the leading ellipsis.
  std::size_t tailBudget = maxWidth - 1;
  // Advance from the start until the remaining tail fits in tailBudget.
  std::size_t i = 0;
  std::size_t dropped = 0;
  while (i < s.size() && total - dropped > tailBudget) {
    auto info = charInfoAt(s.data() + i, s.size() - i);
    dropped += info.width;
    i += info.len;
  }
  // U+2026 HORIZONTAL ELLIPSIS (width 1, 3 bytes UTF-8).
  std::string out = "\xe2\x80\xa6";
  out.append(s.data() + i, s.size() - i);
  return out;
}

void readUtf8Tail(unsigned char lead, std::string& dest) {
  for (std::size_t j = 1; j < utf8SeqLen(lead); ++j) {
    char next;
    if (read(STDIN_FILENO, &next, 1) != 1)
      break;
    dest += next;
  }
}

std::size_t visualColRange(std::string const& line, std::size_t from, std::size_t to) {
  std::size_t vcol = 0;
  for (std::size_t i = from; i < to && i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    vcol += info.width;
    i += info.len;
  }
  return vcol;
}

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

VisualRowInfo findVisualRow(std::string const& line, std::size_t byteOff, std::size_t cols) {
  std::size_t prevStart = 0, curStart = 0;
  std::size_t col = 0;

  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
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

std::size_t lastVisualRowStart(std::string const& line, std::size_t cols) {
  std::size_t rowStart = 0, col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
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

std::vector<std::size_t> computeRowBounds(std::string const& line, std::size_t cols) {
  std::vector<std::size_t> rowStarts;
  rowStarts.push_back(0);

  std::size_t col = 0;
  for (std::size_t i = 0; i < line.size();) {
    auto info = charInfoAt(line.data() + i, line.size() - i);
    if (col + info.width > cols) {
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

uint32_t readUtf8Codepoint(unsigned char lead) {
  if (lead < 0x80)
    return lead;
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
