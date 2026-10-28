#include "version.hpp"

#include "build_info.hpp"

std::string versionText() {
  std::string out = std::string("cx ") + build_info::commit + "\n";
  out += std::string("built ") + build_info::date + " with " + build_info::compiler + "\n";
  if (*build_info::buildType)
    out += std::string("build type: ") + build_info::buildType + "\n";
  out += std::string("flags: ") + build_info::flags + "\n";
  return out;
}
