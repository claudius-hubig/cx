#include "error_log.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <unistd.h>

void logError(std::string_view msg) {
  const char *home = std::getenv("HOME");
  if (!home)
    return;

  std::filesystem::path logPath = std::filesystem::path(home) / ".cx-errors";
  std::ofstream file(logPath, std::ios::app);
  if (!file)
    return;

  file << "[" << std::chrono::system_clock::now() << " pid=" << getpid() << "] " << msg << "\n";
  file.flush();
}
