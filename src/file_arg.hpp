#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Parse a command-line file argument that may carry a trailing line
// specifier (`path:LINE`). Used by main() so users can run
// `cx src/foo.cpp:42` and land on line 42 directly.
//
// Rules:
// - Split at the LAST ':' in the argument.
// - The suffix must be a non-empty all-decimal-digit positive integer
//   for the split to be honoured. Trailing junk (e.g. ":42:") falls
//   back to "path = whole arg, no line".
// - If the un-split argument already names an existing file or
//   directory, treat it as a path (handles filenames containing ':').
// - A line of 0 is treated as no-line; cx is 1-based for line numbers.
inline std::pair<std::string, std::optional<std::size_t>> parseFileArg(std::string_view arg) {
  std::pair<std::string, std::optional<std::size_t>> none{std::string(arg), std::nullopt};
  // If the literal arg exists on disk, do not split — preserve filenames
  // containing ':'.
  std::error_code ec;
  if (!arg.empty() && std::filesystem::exists(std::filesystem::path(arg), ec))
    return none;
  auto colon = arg.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= arg.size())
    return none;
  auto suffix = arg.substr(colon + 1);
  std::size_t line = 0;
  for (char c : suffix) {
    if (c < '0' || c > '9')
      return none;
    auto d = static_cast<std::size_t>(c - '0');
    // Cap at SIZE_MAX rather than overflowing — a buffer with that many
    // lines is impossible, and cx clamps gotoLine to the buffer length.
    if (line > (static_cast<std::size_t>(-1) - d) / 10)
      return none;
    line = line * 10 + d;
  }
  if (line == 0)
    return none;
  return {std::string(arg.substr(0, colon)), line};
}
