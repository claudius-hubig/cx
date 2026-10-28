#pragma once

/**
 * @file version.hpp
 * @brief The `cx --version` text: git commit, build date, compiler and
 *        compile flags.
 *
 * Defined in version.cpp, which is compiled into the cx binary only (it
 * includes the build-generated build_info.hpp), so library and test
 * targets are not rebuilt when the commit changes.
 */

#include <string>

/** @brief Multi-line, newline-terminated version and build information. */
std::string versionText();
