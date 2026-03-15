#include "error_log.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <unistd.h>

void logError(std::string_view msg) {
    const char* home = std::getenv("HOME");
    if (!home) return;

    std::filesystem::path logPath = std::filesystem::path(home) / ".cx-errors";
    std::ofstream file(logPath, std::ios::app);
    if (!file) return;

    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_r(&time, &tm_buf);
    std::ostringstream ts;
    ts << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");

    file << "[" << ts.str() << " pid=" << getpid() << "] " << msg << "\n";
    file.flush();
}
