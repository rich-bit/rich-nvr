#include "Settings.h"
#include <fstream>

Settings::Settings(const std::string &json_path)
    : json_path_(json_path), defaults_() {
  reload();
}

void Settings::reload() {
  std::ifstream f(json_path_);
  if (f)
    f >> json_;
}

void Settings::save() const {
  std::ofstream f(json_path_);
  if (f)
    f << json_.dump(4);
}

// -------- Live RTSP proxy port --------
int Settings::live_rtsp_proxy_port() const {
  if (json_.contains("live_rtsp_proxy_port"))
    return json_["live_rtsp_proxy_port"];
  return defaults_.live_rtsp_proxy_port_;
}

// ------- SEGMENT/PROXY -------------
int Settings::segment_bitrate() const {
  if (json_.contains("segment_bitrate"))
    return json_["segment_bitrate"];
  return defaults_.segment_bitrate;
}
std::string Settings::segment_speedpreset() const {
  if (json_.contains("segment_speedpreset"))
    return json_["segment_speedpreset"];
  return defaults_.segment_speedpreset;
}
int Settings::proxy_bitrate() const {
  if (json_.contains("proxy_bitrate"))
    return json_["proxy_bitrate"];
  return defaults_.proxy_bitrate;
}
std::string Settings::proxy_speedpreset() const {
  if (json_.contains("proxy_speedpreset"))
    return json_["proxy_speedpreset"];
  return defaults_.proxy_speedpreset;
}

// -------- MOTION FRAME SIZE --------
IntSize Settings::motionFrameSize() const {
  if (json_.contains("motion_frame_size") &&
      json_["motion_frame_size"].is_array()) {
    auto arr = json_["motion_frame_size"];
    if (arr.size() == 2)
      return IntSize{static_cast<int>(arr[0]), static_cast<int>(arr[1])};
  }
  return defaults_.motion_frame_size_;
}

float Settings::motion_frame_scale() const {
  if (json_.contains("motion_frame_scale"))
    return json_["motion_frame_scale"];
  return defaults_.motion_frame_scale_;
}

// -------- MOTION ANALYSIS PARAMS ---------
float Settings::noise_threshold() const {
  if (json_.contains("noise_threshold"))
    return json_["noise_threshold"];
  return defaults_.noise_threshold_;
}
float Settings::motion_threshold() const {
  if (json_.contains("motion_threshold"))
    return json_["motion_threshold"];
  return defaults_.motion_threshold_;
}
int Settings::motion_min_hits() const {
  if (json_.contains("motion_min_hits"))
    return json_["motion_min_hits"];
  return defaults_.motion_min_hits_;
}
int Settings::motion_decay() const {
  if (json_.contains("motion_decay"))
    return json_["motion_decay"];
  return defaults_.motion_decay_;
}
float Settings::motion_arrow_scale() const {
  if (json_.contains("motion_arrow_scale"))
    return json_["motion_arrow_scale"];
  return defaults_.motion_arrow_scale_;
}
int Settings::motion_arrow_thickness() const {
  if (json_.contains("motion_arrow_thickness"))
    return json_["motion_arrow_thickness"];
  return defaults_.motion_arrow_thickness_;
}

// -------- VIDEO OUTPUT FORMAT ---------
std::string Settings::video_output_format() const {
  if (json_.contains("video_output_format"))
    return json_["video_output_format"];
  return defaults_.video_output_format_;
}

// -------- Templated Setter ---------
template <typename T>
void Settings::set(const std::string &key, const T &value) {
  json_[key] = value;
  save();
  reload();
}

// Explicit template instantiations if used in multiple TUs
template void Settings::set<int>(const std::string &, const int &);
template void Settings::set<std::string>(const std::string &,
                                         const std::string &);
template void Settings::set<double>(const std::string &, const double &);
template void Settings::set<float>(const std::string &, const float &);
