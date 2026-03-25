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
