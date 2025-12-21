#include "VideoExporter.h"
#include <QFile>
#include <QProcess>
#include <QStringList>
#include <QTextStream>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QTextStream>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

bool VideoExporter::exportSegments(
    const std::vector<std::filesystem::path> &segmentsIn,
    const fs::path &outputFolder, const std::string &outputFilenameIn) {

  if (segmentsIn.empty()) {
    std::cerr << "[VideoExporter] No segments to export.\n";
    return false;
  }

  // Filter out zero/small files just in case
  std::vector<fs::path> segments;
  segments.reserve(segmentsIn.size());
  for (const auto &p : segmentsIn) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (!ec && sz > 1024)
      segments.push_back(p);
  }
  if (segments.empty()) {
    std::cerr << "[VideoExporter] All segments are empty/too small.\n";
    return false;
  }

  // Choose output filename: default to .mkv if caller passed no extension
  std::string outputFilename = outputFilenameIn;
  if (fs::path(outputFilename).extension().empty())
    outputFilename += ".mkv"; // keep Matroska end-to-end

  // Write concat list
  fs::path listFile = outputFolder / "concat_list.txt";
  QFile file(QString::fromStdString(listFile.string()));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    std::cerr << "[VideoExporter] Failed to create concat list.\n";
    return false;
  }
  auto escapeForConcat = [](const std::string &s) {
    // concat demuxer expects: file 'path' — single quotes inside must be
    // escaped: '\''
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      if (c == '\'')
        out += "'\\''";
      else
        out += c;
    }
    return out;
  };
  QTextStream out(&file);
  for (const auto &seg : segments) {
    out << "file '" << QString::fromStdString(escapeForConcat(seg.string()))
        << "'\n";
  }
  file.close();

  // Build ffmpeg args
  QStringList args;
  args << "-y" << "-f" << "concat" << "-safe" << "0" << "-i"
       << QString::fromStdString(listFile.string())
       // Force Matroska on output if extension is .mkv (optional but explicit)
       << "-c" << "copy";

  fs::path outputPath = outputFolder / outputFilename;
  if (fs::path(outputFilename).extension() == ".mkv") {
    args << "-f" << "matroska";
  }
  args << QString::fromStdString(outputPath.string());

  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", args);
  bool started = ffmpeg.waitForStarted();
  bool finished = ffmpeg.waitForFinished(-1);

  if (!started || !finished || ffmpeg.exitCode() != 0) {
    std::cerr << "[VideoExporter] FFmpeg failed: "
              << ffmpeg.readAllStandardError().toStdString() << std::endl;
    return false;
  }

  std::cout << "[VideoExporter] Export completed: " << outputPath << std::endl;

  // Cleanup concat list
  QFile::remove(QString::fromStdString(listFile.string()));

  // Delete original segments (only if you truly want to remove them)
  cleanupSegments(segments);

  return true;
}

void VideoExporter::cleanupSegments(
    const std::vector<std::filesystem::path> &segments) {
  for (const auto &seg : segments) {
    QFile qfile(QString::fromStdString(seg.string()));
    if (!qfile.remove()) {
      std::cerr << "[VideoExporter] Failed to delete: " << seg << std::endl;
    } else {
      std::cout << "[VideoExporter] Deleted: " << seg << std::endl;
    }
  }
}
