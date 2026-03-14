#pragma once

#include <string_view>

// Append a timestamped error line to ~/.cx-errors.
// Safe to call at any point, including from destructors.
void logError(std::string_view msg);
