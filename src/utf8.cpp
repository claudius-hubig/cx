#include "utf8.hpp"

#include <unistd.h>

void readUtf8Tail(unsigned char lead, std::string& dest) {
  for (std::size_t j = 1; j < utf8SeqLen(lead); ++j) {
    char next;
    if (read(STDIN_FILENO, &next, 1) != 1)
      break;
    dest += next;
  }
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
