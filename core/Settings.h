#pragma once
#include "Defaults.h"
#include "Types.h"
#include <nlohmann/json.hpp>
#include <string>

class Settings {
public:
  Settings(const std::string &json_path);

  // Live proxied
  int live_rtsp_proxy_port() const;

  // Segment/Proxy
  int segment_bitrate() const;
  std::string segment_speedpreset() const;
  int proxy_bitrate() const;
  std::string proxy_speedpreset() const;

  // Motion frame sizing/scaling
  IntSize motionFrameSize() const;
  float motion_frame_scale() const;

  // Motion analysis parameters
  float noise_threshold() const;
  float motion_threshold() const;
  int motion_min_hits() const;
  int motion_decay() const;
  float motion_arrow_scale() const;
  int motion_arrow_thickness() const;

  // Video output
  std::string video_output_format() const;

  // Generic setter
  template <typename T> void set(const std::string &key, const T &value);

private:
  void reload();
  void save() const;

  std::string json_path_;
  SettingsDefaults defaults_;
  nlohmann::json json_;
};
