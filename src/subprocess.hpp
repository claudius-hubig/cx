#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

struct SubprocessResult {
  int exitCode;
  std::string stdoutData;
  std::string stderrData;
};

// Run a command with the given arguments, feeding input to its stdin.
// Returns stdout, stderr, and exit code on success, or an error string
// if the process could not be started.
std::expected<SubprocessResult, std::string> runSubprocess(std::vector<std::string> const& args,
                                                           std::string_view input);
