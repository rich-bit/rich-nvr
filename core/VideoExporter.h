// VideoExporter.h
#pragma once

#include <filesystem>
#include <string>
#include <vector>

class VideoExporter {
public:
  // Exports all segments (including and before lastSavedSegment) into
  // outputFilename
  static bool exportSegments(const std::vector<std::filesystem::path> &segments,
                             const std::filesystem::path &outputFolder,
                             const std::string &outputFilename);

private:
  static void
  cleanupSegments(const std::vector<std::filesystem::path> &segments);
};
