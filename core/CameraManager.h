#pragma once

#include "CameraStream.h"
#include "Settings.h"
#include "gstreamerRtspProxy.h"
#include "live555RtspProxy.h"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CameraManager {
public:
  CameraManager(Settings &settings);
  ~CameraManager();

  void removeCamera(const std::string &name);
  CameraStream *getCamera(const std::string &name);

  void startAll();
  void stopAll();
  void addCamera(const std::string &name, const std::string &uri, bool segment,
                 bool recording, bool overlay, bool motion_frame,
                 bool gstreamerEncodedProxy, bool live555proxied, bool loading,
                 int segment_bitrate, const std::string &segment_speed_preset,
                 int proxy_bitrate, const std::string &proxy_speed_preset,
                 cv::Size motion_frame_size, float motion_frame_scale,
                 float noise_threshold, float motion_threshold,
                 int motion_min_hits, int motion_decay,
                 float motion_arrow_scale, int motion_arrow_thickness,
                 std::string video_output_format,
                 std::optional<AudioProbeResult> audio_hint = std::nullopt);

  std::vector<std::string> getCameraNames() const;

  // Load previously added cameras when cameramanager is created
  std::string config_path_;
  void loadCamerasFromJSON(const std::string &path);

  // JSON array with one object per camera (see implementation for fields)
  nlohmann::json getCamerasInfoJson() const;

  // Motion region management
  int addMotionRegionToCamera(const std::string &cameraId,
                              const cv::Rect &region, float angle = 0.0f);
  bool removeMotionRegionFromCamera(const std::string &cameraId, int regionId);
  void clearMotionRegionsFromCamera(const std::string &cameraId);
  std::vector<MotionRegion>
  getMotionRegionsFromCamera(const std::string &cameraId) const;

  // Save camera settings to JSON
  void saveSingleCameraToJSON(const std::string &filename,
                              const std::string &cameraName);

private:
  std::map<std::string, std::unique_ptr<CameraStream>> cameras_;

  GstreamerRtspProxy gstreamer_proxy_;

  Settings &settings_;
  // Live555 RTSP proxy server
  live555RtspProxy live555_proxy_;
  uint16_t live555_port_ = 8554; // optional: expose/configure elsewhere
  std::string live555_bind_host_ = "127.0.0.1";

  void saveCamerasToJSON(const std::string &filename);
};
