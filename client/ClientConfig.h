#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ConfigurationPanel.h"

namespace client_config {

extern const char kClientConfigFileName[];
extern const char kUnknownCameraName[];

struct CameraConfig {
    std::string name;
    std::string ip;
    bool via_server = false;
    std::string original_uri;
    bool segment = false;
    bool recording = false;
    bool overlay = false;
    bool motion_frame = false;
    bool gstreamer_proxy = false;
    bool live555_proxy = false;
    int segment_bitrate = 0;
    std::string segment_speed_preset;
    int proxy_bitrate = 0;
    std::string proxy_speed_preset;
    int motion_frame_width = 0;
    int motion_frame_height = 0;
    float motion_frame_scale = 0.0f;
    float noise_threshold = 0.0f;
    float motion_threshold = 0.0f;
    int motion_min_hits = 0;
    int motion_decay = 0;
    float motion_arrow_scale = 0.0f;
    int motion_arrow_thickness = 0;
};

struct ClientConfig {
    std::string server_ip;
    std::string server_endpoint;
    std::vector<CameraConfig> cameras;
    ConfigurationWindowSettings window_settings;
};

ClientConfig create_default_client_config(nlohmann::json &json_doc);
std::filesystem::path resolve_config_path(const char *argv0);
ClientConfig load_client_config(const std::filesystem::path &config_path, nlohmann::json &out_json);
void save_client_config(const nlohmann::json &json_doc, const std::filesystem::path &config_path);
void sync_json_from_client_config(nlohmann::json &json_doc, const ClientConfig &config);

} // namespace client_config
