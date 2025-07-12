// SettingsDefaults
#pragma once

#include "Types.h"
#include <string>

struct SettingsDefaults {
  std::string segment_speedpreset = "veryfast";
  int segment_bitrate = 2000;
  std::string proxy_speedpreset = "superfast";
  int proxy_bitrate = 2000;
  IntSize motion_frame_size_{0, 0};
  float noise_threshold_ = 1.0f;
  float motion_threshold_ = 10.0f;
  int motion_min_hits_ = 3;
  int motion_decay_ = 1;
  float motion_arrow_scale_ = 2.5f;
  int motion_arrow_thickness_ = 1;
  float motion_frame_scale_ = 1.0f;
  std::string video_output_format_ = "mkv";
  int live_rtsp_proxy_port_ = 8554;
};
