#pragma once

#include <string_view>

// Append a timestamped error line to ~/.cx-errors.
// Safe to call at any point, including from destructors. NOT
// async-signal-safe — opens a std::ofstream, formats a chrono
// timestamp, and uses std::filesystem::path internally. Do not call
// from a signal handler.
void logError(std::string_view msg);
