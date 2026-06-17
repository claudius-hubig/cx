#pragma once

/**
 * @file error_log.hpp
 * @brief Diagnostic logging to ~/.cx-errors for post-mortem inspection.
 */

#include <string_view>

/**
 * @brief Append a timestamped error line to ~/.cx-errors.
 *
 * Safe to call at any point, including from destructors. NOT
 * async-signal-safe — it opens a std::ofstream, formats a chrono
 * timestamp, and uses std::filesystem::path internally — so it must not
 * be called from a signal handler.
 *
 * @param msg The message to record.
 */
void logError(std::string_view msg);
