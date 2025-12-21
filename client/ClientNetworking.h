#pragma once

#include <string>

#include "ConfigurationPanel.h"

namespace client_network {

std::string extract_host_from_endpoint(const std::string &endpoint);
std::string build_proxy_rtsp_url(const std::string &endpoint,
                                 const std::string &camera_name);
AddCameraResult send_add_camera_request(const AddCameraRequest &request,
                                        std::string &response_body);
bool toggle_motion_detection(const std::string &endpoint,
                             const std::string &camera_name, bool enable);
bool fetch_motion_frame_jpeg(const std::string &endpoint,
                             const std::string &camera_name,
                             std::vector<unsigned char> &jpeg_data);
int add_motion_region(const std::string &endpoint,
                      const std::string &camera_name, int x, int y, int w,
                      int h, float angle);
bool remove_motion_region(const std::string &endpoint,
                          const std::string &camera_name, int region_id);
bool clear_motion_regions(const std::string &endpoint,
                          const std::string &camera_name);
std::vector<ConfigurationPanel::MotionRegion>
get_motion_regions(const std::string &endpoint, const std::string &camera_name);
bool update_camera_properties(const std::string &endpoint,
                              const std::string &camera_name,
                              float motion_frame_scale, float noise_threshold,
                              float motion_threshold, int motion_min_hits,
                              int motion_decay, float motion_arrow_scale,
                              int motion_arrow_thickness);
bool toggle_segment_recording(const std::string &endpoint,
                              const std::string &camera_name, bool enable);
bool remove_camera(const std::string &endpoint, const std::string &camera_name);
std::vector<ConfigurationPanel::CameraInfo>
get_cameras_from_server(const std::string &endpoint);

struct ServerHealthInfo {
  bool available;
  int http_port;
  int rtsp_proxy_port;
  int camera_count;
  long uptime_seconds;
  std::string error_message;
};

struct ServerThreadInfo {
  std::string name;
  bool is_active;
  std::string details;
};

ServerHealthInfo check_server_health(const std::string &endpoint);
std::vector<ServerThreadInfo> get_server_threads(const std::string &endpoint);

} // namespace client_network
