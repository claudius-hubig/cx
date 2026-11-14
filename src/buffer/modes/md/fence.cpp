#include "buffer/modes/md/fence.hpp"

#include <string_view>

std::vector<bool> markdownFencedLines(LineStore const& lines) {
  std::vector<bool> fenced(lines.size(), false);
  char fenceChar = 0; // 0 outside a fence
  std::size_t fenceLen = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    std::string_view line = *lines[i];
    auto start = line.find_first_not_of(" \t");
    char c = start == std::string_view::npos ? 0 : line[start];
    std::size_t run = 0;
    if (c == '`' || c == '~') {
      auto end = line.find_first_not_of(c, start);
      run = (end == std::string_view::npos ? line.size() : end) - start;
    }
    std::string_view rest = run ? line.substr(start + run) : std::string_view{};
    if (fenceChar == 0) {
      if (run >= 3 && (c == '~' || rest.find('`') == std::string_view::npos)) {
        fenceChar = c;
        fenceLen = run;
        fenced[i] = true;
      }
    } else {
      fenced[i] = true;
      if (c == fenceChar && run >= fenceLen &&
          rest.find_first_not_of(" \t") == std::string_view::npos)
        fenceChar = 0;
    }
  }
  return fenced;
}
