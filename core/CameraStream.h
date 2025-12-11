#pragma once
#include "SegmentWorker.h"
#include "Settings.h"
#include <chrono>
#include <gst/gst.h>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>
#include <vector>

struct AudioProbeResult {
  bool has_audio = false;
  std::string encoding;
  int channels = 0;
  int rate = 0;
  bool probed = false;
};

struct MotionRegion {
  int id;
  cv::Rect rect;
  float angle;
  
  MotionRegion(int id, const cv::Rect& rect, float angle = 0.0f) 
    : id(id), rect(rect), angle(angle) {}
  
  // Get rotated rectangle for point-in-region testing
  cv::RotatedRect getRotatedRect() const {
    return cv::RotatedRect(cv::Point2f(rect.x + rect.width/2.0f, rect.y + rect.height/2.0f),
                          cv::Size2f(rect.width, rect.height), angle);
  }
};

class CameraStream {
public:
  CameraStream(const std::string &name, const std::string &uri,
               Settings &settings, bool segment = false, bool recording = false,
               bool overlay = false, bool motion_frame = false,
               bool gstreamerEncodedProxy = false, bool live555Proxied_ = false,
               int proxy_bitrate_ = 2000,
               std::string proxy_speed_preset_ = "superfast",
               int segment_bitrate_ = 2000,
               std::string segment_speed_preset_ = "veryfast",
               cv::Size motion_frame_size = cv::Size(0, 0),
               float motion_frame_scale = 1.0f,
               float noise_threshold = 0.0f, float motion_threshold = 0.0f,
               int motion_min_hits = 1, int motion_decay = 0,
               float motion_arrow_scale = 2.5f, int motion_arrow_thickness = 1,
               std::string video_output_format = "mp4");

  ~CameraStream();

  void start();
  void stop();

  // Feature toggles
  void enableSegmentRecording();
  void disableSegmentRecording();
  void enableFullRecording(const std::string &filename);
  void disableFullRecording();
  void enableTimestampOverlay();
  void disableTimestampOverlay();
  void enableMotionFrameSaving(const std::string &outPath);
  void disableMotionFrameSaving();

  std::string getMountPoint() const;

  // Getters/setters
  const AudioProbeResult &audioProbe() const { return pr_; }
  bool hasAudioHint() const { return pr_.has_audio; }
  void setAudioHint(const AudioProbeResult &r) { pr_ = r; }
  const std::string &name() const { return name_; }
  const std::string &uri() const { return uri_; }
  bool segment() const { return segment_; }
  bool recording() const { return recording_; }
  bool overlay() const { return overlay_; }
  bool motion_frame() const { return motion_frame_; }
  bool getGstreamerEncodedProxy() const { return gstreamerEncodedProxy_; }
  bool getLive555Proxied() const { return live555Proxied_; }
  int getProxyBitrate() const { return proxy_bitrate_; }
  const std::string &getProxySpeedPreset() const { return proxy_speed_preset_; }
  int getSegmentBitrate() const { return segment_bitrate_; }
  const std::string &getSegmentSpeedPreset() const {
    return segment_speed_preset_;
  }
  // Returns the last detected motion frame as a JPEG buffer
  const std::vector<uchar> &getLastMotionJpeg() const { return last_jpeg_buf_; }
  const cv::Mat &getLastMotionFrame() const { return last_motion_frame_; }
  void setMotionFrameSize(const cv::Size &sz) { motion_frame_size_ = sz; }
  cv::Size getMotionFrameSize() const { return motion_frame_size_; }

  void setMotionFrameScale(float s) { motion_frame_scale_ = s; }
  float getMotionFrameScale() const { return motion_frame_scale_; }

  void setNoiseThreshold(float t) { noise_threshold_ = t; }
  float getNoiseThreshold() const { return noise_threshold_; }

  void setMotionThreshold(float t) { motion_threshold_ = t; }
  float getMotionThreshold() const { return motion_threshold_; }

  void setMotionMinHits(int h) { motion_min_hits_ = h; }
  int getMotionMinHits() const { return motion_min_hits_; }

  void setMotionDecay(int d) { motion_decay_ = d; }
  int getMotionDecay() const { return motion_decay_; }

  void setMotionArrowScale(float s) { motion_arrow_scale_ = s; }
  float getMotionArrowScale() const { return motion_arrow_scale_; }

  void setMotionArrowThickness(int t) { motion_arrow_thickness_ = t; }
  int getMotionArrowThickness() const { return motion_arrow_thickness_; }

  void setVideoOutputFormat(const std::string &fmt) {
    video_output_format_ = fmt;
  }
  const std::string &getVideoOutputFormat() const {
    return video_output_format_;
  }

  // Motion region management
  int addMotionRegion(const cv::Rect& rect, float angle = 0.0f);
  bool removeMotionRegion(int id);
  void clearMotionRegions();
  const std::vector<MotionRegion>& getMotionRegions() const { return motion_regions_; }

private:
  std::string buildPipelineWithAudio() const;
  std::string buildPipelineWithoutAudio() const;
  void startMotionLoop();
  void rebuild();
  void exportInBackground(const std::vector<std::filesystem::path> &segments,
                          const std::filesystem::path &outputFolder,
                          const std::string &outputFilename);
  std::string getTimestampedFilename(const std::string &prefix = "motion-",
                                     const std::string &ext = ".mkv");

  static bool ProbeRtspAudio(const std::string &uri, AudioProbeResult &out,
                             int timeout_ms = 1500);

  AudioProbeResult pr_;

  std::unique_ptr<SegmentWorker> segmentWorker_;

  GstElement *motion_sink_ = nullptr;
  std::thread motion_thread_;

  using Clock = std::chrono::steady_clock;
  std::chrono::steady_clock::time_point lastMotionTime_;
  float noise_threshold_ = 1.0f;
  float motion_threshold_ = 0.0f;
  int motion_hold_duration_ = 5; // s
  int motion_min_hits_ = 3;
  int motion_decay_ = 1;
  float motion_arrow_scale_ = 2.5f;
  int motion_arrow_thickness_ = 1;
  float motion_frame_scale_ = 1.0f;
  bool motionDetected_ = false;
  bool prevMotionDetected_ = false;
  std::string video_output_format_ = "mp4";
  std::string output_path_;

  cv::Mat last_motion_frame_;
  cv::Size motion_frame_size_{0, 0};
  std::vector<uchar> last_jpeg_buf_;

  // Motion regions
  std::vector<MotionRegion> motion_regions_;
  int next_region_id_ = 1;

  std::string name_;
  std::string uri_;
  std::string mount_point_;

  void *pipeline_ = nullptr;
  bool running_ = false;
  bool motion_running_ = false;

  // Feature toggles/settings
  std::atomic<bool> segment_{false};
  bool recording_ = false;
  bool gstreamerEncodedProxy_ = false;
  bool live555Proxied_ = false;
  std::string recordFile_;
  bool overlay_ = false;
  bool motion_frame_ = false;
  std::string motionFile_;
  Settings &settings_;
  int segment_bitrate_;
  std::string segment_speed_preset_;
  int proxy_bitrate_;
  std::string proxy_speed_preset_;
  std::string segment_path;
};
