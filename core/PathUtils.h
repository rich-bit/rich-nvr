#pragma once

#include <string>

namespace core {

class PathUtils {
public:
  // Returns the directory where the currently-running executable is located.
  static std::string getExecutableDir();
  static void ensureDirExists(const std::string &path);
  static std::string sanitizeCameraName(const std::string &name);

  // Platform detection utilities
  static bool isWSLEnvironment();
};

} // namespace core
