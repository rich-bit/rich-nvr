#include "SegmentWorker.h"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

SegmentWorker::SegmentWorker(const std::string &segmentPath, int msUpdate)
    : segmentPath_(segmentPath), msUpdate_(msUpdate), running_(false),
      state_(WorkerState::Stopped) {}

void SegmentWorker::start() {
  if (running_)
    return;

  // Construct savedPath_
  savedPath_ = (fs::path(segmentPath_) / "saved").string();

  try {
    // Create directory if it doesn't exist
    if (!fs::exists(savedPath_)) {
      fs::create_directories(savedPath_);
    }
  } catch (const std::exception &ex) {
    std::cerr << "[SegmentWorker] Failed to create saved dir: " << ex.what()
              << std::endl;
  }

  running_ = true;
  state_ = WorkerState::Working;

  workerThread_ = std::thread([this]() { scanSegmentDir(); });

  std::cout << "[SegmentWorker] Started." << std::endl;
}

void SegmentWorker::stop() {
  running_ = false;
  if (workerThread_.joinable()) {
    workerThread_.join();
  }

  std::cout << "[SegmentWorker] Worker stopped." << std::endl;

  state_ = WorkerState::Stopped;
}
void SegmentWorker::SaveCurrentSegment() {
  std::lock_guard<std::mutex> lock(saveMutex_);
  if (saveCurrentSegment_)
    return;

  saveCurrentSegment_ = true;
}

void SegmentWorker::setState(WorkerState newState) {
  state_.store(newState, std::memory_order_relaxed);
}

SegmentWorker::WorkerState SegmentWorker::getState() const {
  return state_.load(std::memory_order_relaxed);
}

std::vector<std::filesystem::path> SegmentWorker::getAndResetMotionSegments() {
  std::lock_guard<std::mutex> lock(saveMutex_);
  std::vector<std::filesystem::path> result = std::move(motionSegments_);
  motionSegments_.clear();
  return result;
}

void SegmentWorker::scanSegmentDir() {
  while (running_) {
    try {
      // Scan segment directory
      fs::file_time_type newestTime;
      std::string newestFile;
      for (const auto &entry : fs::directory_iterator(segmentPath_)) {
        if (!entry.is_regular_file())
          continue;

        std::string filename = entry.path().filename().string();
        if (filename.find(".mkv") == std::string::npos)
          continue;

        auto lastWrite = fs::last_write_time(entry);
        if (newestFile.empty() || lastWrite > newestTime) {
          newestTime = lastWrite;
          newestFile = filename;
        }
      }
      currentSegment_ = newestFile;
      if (currentSegment_ != savedSegment_) {
        // std::cout << "[SegmentWorker] Segment switch to " << currentSegment_
        //           << std::endl;

        bool shouldSave = false;
        {
          std::lock_guard<std::mutex> lock(saveMutex_);
          if (saveCurrentSegment_) {
            shouldSave = true;
          }
        }

        // Only save if we have a valid previous segment filename
        if (shouldSave && !savedSegment_.empty()) {
          bool success = false;
          fs::path outputFilename;
          try {
            fs::path src = fs::path(segmentPath_) / savedSegment_;

            // Verify source file exists and is a regular file
            if (!fs::exists(src)) {
              std::cerr << "[SegmentWorker] Source file does not exist: "
                        << src << std::endl;
              throw std::runtime_error("Source file not found");
            }
            if (!fs::is_regular_file(src)) {
              std::cerr << "[SegmentWorker] Source is not a regular file: "
                        << src << std::endl;
              throw std::runtime_error("Source is not a file");
            }

            // Get current time
            auto now = std::chrono::system_clock::now();
            std::time_t now_c = std::chrono::system_clock::to_time_t(now);

            // Format time as YYYY-MM-DD_HH-MM-SS
            std::tm tm;
            localtime_r(&now_c, &tm); // or localtime_s on Windows
            char buffer[64];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H-%M-%S", &tm);

            // Build new filename
            fs::path dstFilename = std::string(buffer) + ".mkv";
            outputFilename = dstFilename;
            fs::path dst = fs::path(savedPath_) / dstFilename;

            fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
            std::cout << "[SegmentWorker] Saved segment to " << dst
                      << std::endl;
            success = true;
          } catch (const std::exception &ex) {
            std::cerr << "[SegmentWorker] Failed to copy segment: " << ex.what()
                      << std::endl;
          }

          if (success) {
            std::lock_guard<std::mutex> lock(saveMutex_);
            motionSegments_.push_back(fs::path(savedPath_) / outputFilename);
            saveCurrentSegment_ = false;
            if (getState() == WorkerState::FinishRequested) {
              std::cerr << "[SegmentWorker] Reporting finished" << std::endl;
              setState(WorkerState::Finalized);
            }
          }
        }
        savedSegment_ = currentSegment_;
      }

    } catch (const std::exception &ex) {
      std::cerr << "[SegmentWorker] Error scanning dir: " << ex.what()
                << std::endl;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(msUpdate_));
  }
}
