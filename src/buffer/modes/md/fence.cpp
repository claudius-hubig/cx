#include "buffer/modes/md/fence.hpp"

#include <algorithm>
#include <limits>

bool MdFence::advance(std::string_view line) {
  auto start = line.find_first_not_of(" \t");
  char c = start == std::string_view::npos ? 0 : line[start];
  std::size_t run = 0;
  if (c == '`' || c == '~') {
    auto end = line.find_first_not_of(c, start);
    run = (end == std::string_view::npos ? line.size() : end) - start;
  }
  std::string_view rest = run ? line.substr(start + run) : std::string_view{};
  if (!open()) {
    if (run < 3 || (c == '`' && rest.find('`') != std::string_view::npos))
      return false;
    ch = c;
    len = static_cast<std::uint16_t>(
        std::min<std::size_t>(run, std::numeric_limits<std::uint16_t>::max()));
    return true;
  }
  if (c == ch && run >= len && rest.find_first_not_of(" \t") == std::string_view::npos)
    *this = {};
  return true;
}

std::vector<bool> markdownFencedLines(LineStore const& lines) {
  std::vector<bool> fenced(lines.size(), false);
  MdFence fence;
  for (std::size_t i = 0; i < lines.size(); ++i)
    fenced[i] = fence.advance(*lines[i]);
  return fenced;
}
