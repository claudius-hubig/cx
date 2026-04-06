#include "utf8.hpp"

#include <algorithm>
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
