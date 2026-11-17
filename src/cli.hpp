#pragma once

/**
 * @file cli.hpp
 * @brief Command-line option parsing for the cx binary: -h/--help,
 *        --version, --theme, and the file/directory arguments.
 */

#include "theme.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cli {

/** @brief Print the usage text and exit. */
struct ShowHelp {};
/** @brief Print version and build information and exit. */
struct ShowVersion {};
/** @brief Start the editor on these arguments (raw, possibly `path:LINE`). */
struct OpenFiles {
  std::vector<std::string> args;
  Theme theme = Theme::Dark; /**< From --theme; the last one given wins. */
};

/** @brief What the command line asks cx to do. */
using Command = std::variant<ShowHelp, ShowVersion, OpenFiles>;

/**
 * @brief Parse @p args (argv without the program name).
 *
 * -h/--help and --version take effect as soon as they are seen, so the
 * first of them wins. --theme=light|dark sets OpenFiles::theme; any other
 * value is an error. `--` ends option parsing: every later argument is a
 * file, which is how to open a file whose name starts with '-'. A lone
 * "-" is a file name too. Any other argument starting with '-' is an
 * error rather than a file, so a mistyped option never silently opens a
 * new buffer.
 *
 * @return The command, or an error message naming the bad option.
 */
inline std::expected<Command, std::string> parse(std::vector<std::string_view> const& args) {
  OpenFiles files;
  bool optionsDone = false;
  for (auto arg : args) {
    if (!optionsDone && arg.size() > 1 && arg.front() == '-') {
      if (arg == "-h" || arg == "--help")
        return ShowHelp{};
      if (arg == "--version")
        return ShowVersion{};
      if (arg == "--") {
        optionsDone = true;
        continue;
      }
      if (arg.starts_with("--theme=")) {
        auto value = arg.substr(8);
        if (value == "dark")
          files.theme = Theme::Dark;
        else if (value == "light")
          files.theme = Theme::Light;
        else
          return std::unexpected("invalid theme '" + std::string(value) +
                                 "' (expected 'light' or 'dark')");
        continue;
      }
      return std::unexpected("unrecognised option '" + std::string(arg) + "'");
    }
    files.args.emplace_back(arg);
  }
  return files;
}

/** @brief The --help text; @p prog is the name cx was invoked as. */
inline std::string usage(std::string_view prog) {
  return "Usage: " + std::string(prog) +
         " [OPTION]... [FILE[:LINE] | DIRECTORY]...\n"
         "\n"
         "Open each FILE for editing and each DIRECTORY for browsing. A trailing\n"
         ":LINE opens the last FILE at that 1-based line. With no arguments, cx\n"
         "starts with an empty scratch buffer, or with the piped text if stdin\n"
         "is a pipe.\n"
         "\n"
         "  -h, --help           show this help and exit\n"
         "      --version        show the git commit and build information and exit\n"
         "      --theme=THEME    colours for a 'dark' (default) or 'light'\n"
         "                       terminal background\n"
         "  --                   treat every following argument as a file\n";
}

} // namespace cli
