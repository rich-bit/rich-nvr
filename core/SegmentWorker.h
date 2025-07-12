#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class SegmentWorker {
public:
  enum class WorkerState { Stopped, Working, FinishRequested, Finalized };

  SegmentWorker(const std::string &segmentPath, int msUpdate = 500);

  void start();
  void stop();
  void SaveCurrentSegment();
  void setState(WorkerState newState);
  WorkerState getState() const;
  std::vector<std::filesystem::path> getAndResetMotionSegments();

private:
  void scanSegmentDir(); // Called by thread in start()

  std::string segmentPath_;
  std::string savedPath_; // Saved segments

  std::vector<std::filesystem::path> motionSegments_;

  int msUpdate_;

  std::thread workerThread_;
  std::atomic<bool> running_;
  bool saveCurrentSegment_ = false;
  std::mutex saveMutex_;

  std::atomic<WorkerState> state_;

  std::string currentSegment_;
  std::string savedSegment_;
};
