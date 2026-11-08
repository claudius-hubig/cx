#pragma once

/**
 * @file fs_path.hpp
 * @brief Non-throwing path helpers for user-supplied paths.
 *
 * The throwing std::filesystem overloads must not be used on paths the
 * user typed or browsed to: a filesystem_error would unwind out of the
 * input loop and end the session with all unsaved work lost.
 */

#include <filesystem>
#include <system_error>

/**
 * @brief Absolute, symlink-resolved form of @p path, as used for buffer keys.
 *
 * Never throws: when resolution fails (an untraversable directory, a
 * symlink loop, ...) falls back to the absolute lexical path, or to @p path
 * itself if even that fails. A later load/save reports the real error.
 */
inline std::filesystem::path canonicalPath(std::filesystem::path const& path) {
  std::error_code ec;
  auto abs = std::filesystem::absolute(path, ec);
  if (ec)
    return path;
  auto canonical = std::filesystem::weakly_canonical(abs, ec);
  return ec ? abs : canonical;
}

/** @brief The process working directory, or "/" if it cannot be determined. */
inline std::filesystem::path currentDir() {
  std::error_code ec;
  auto cwd = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path("/") : cwd;
}
