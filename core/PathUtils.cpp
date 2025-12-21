#include "PathUtils.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#elif defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h>
#else // Linux, BSD, etc
#include <limits.h>
#include <unistd.h>
#endif

namespace core {

std::string PathUtils::getExecutableDir() {
  char path[PATH_MAX];
#if defined(_WIN32)
  if (GetModuleFileNameA(nullptr, path, sizeof(path)) == 0)
    return "";
#elif defined(__APPLE__)
  uint32_t size = sizeof(path);
  if (_NSGetExecutablePath(path, &size) != 0)
    return "";
#elif defined(__linux__)
  ssize_t count = readlink("/proc/self/exe", path, sizeof(path));
  if (count == -1 || count == sizeof(path))
    return "";
  path[count] = '\0';
#else
  return "";
#endif
  std::string fullPath(path);
  return fullPath.substr(0, fullPath.find_last_of("/\\"));
}
void PathUtils::ensureDirExists(const std::string &path) {
  std::filesystem::create_directories(path);
}

std::string PathUtils::sanitizeCameraName(const std::string &name) {
  std::string safe;
  for (char c : name) {
    if (isalnum(c) || c == '_' || c == '-') {
      safe += c;
    }
    // else skip/replace with underscore if you want
  }
  return safe;
}

bool PathUtils::isWSLEnvironment() {
  // Check for WSL by looking at /proc/version or environment
  std::ifstream versionFile("/proc/version");
  if (versionFile.is_open()) {
    std::string version;
    std::getline(versionFile, version);
    if (version.find("WSL") != std::string::npos ||
        version.find("Microsoft") != std::string::npos) {
      return true;
    }
  }
  return std::getenv("WSL_DISTRO_NAME") != nullptr ||
         std::getenv("WSLENV") != nullptr;
}

} // namespace core
