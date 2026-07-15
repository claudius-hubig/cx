#include "error_log.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <unistd.h>

/**
 * @brief Append an error message to the user's error log file.
 *
 * Writes the message with a timestamp and process ID to ~/.cx-errors.
 * If HOME is not set or the file cannot be opened, the error is silently dropped.
 *
 * @param msg The error message to log.
 */
void logError(std::string_view msg) {
  char const* home = std::getenv("HOME");
  if (!home)
    return;

  std::filesystem::path logPath = std::filesystem::path(home) / ".cx-errors";
  std::ofstream file(logPath, std::ios::app);
  if (!file)
    return;

  file << "[" << std::chrono::system_clock::now() << " pid=" << getpid() << "] " << msg << "\n";
  file.flush();
}
