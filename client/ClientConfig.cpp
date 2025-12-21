#include "ClientConfig.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace client_config {

const char kClientConfigFileName[] = "client_config.json";
const char kUnknownCameraName[] = "Camera";

ClientConfig create_default_client_config(nlohmann::json &json_doc) {
  ClientConfig config;
  config.server_endpoint = "http://localhost:8080";
  config.server_ip = "localhost";

  json_doc = nlohmann::json::object();

  auto &configuration_node = json_doc["configuration"];
  configuration_node = nlohmann::json::object();
  auto &general_node = configuration_node["general"];
  general_node = nlohmann::json::object();
  general_node["windowWidth"] = config.window_settings.width;
  general_node["windowHeight"] = config.window_settings.height;
  general_node["showImGuiMetrics"] = config.window_settings.show_imgui_metrics;
  general_node["serverEndpoint"] = config.server_endpoint;

  auto &server_node = json_doc["server"];
  server_node = nlohmann::json::object();
  server_node["serverIP"] = config.server_ip;
  server_node["cameras"] = nlohmann::json::array();

  return config;
}

std::filesystem::path resolve_config_path(const char *argv0) {
  std::filesystem::path candidate;
  if (argv0 && *argv0) {
    std::error_code ec;
    std::filesystem::path executable_path(argv0);
    auto canonical = std::filesystem::weakly_canonical(executable_path, ec);
    if (!ec && canonical.has_parent_path()) {
      candidate = canonical.parent_path();
    } else if (executable_path.has_parent_path()) {
      candidate = executable_path.parent_path();
    }
  }

  if (candidate.empty()) {
    candidate = std::filesystem::current_path();
  }

  return candidate / kClientConfigFileName;
}

ClientConfig load_client_config(const std::filesystem::path &config_path,
                                nlohmann::json &out_json) {
  std::ifstream input(config_path);
  if (!input) {
    ClientConfig default_config = create_default_client_config(out_json);
    try {
      save_client_config(out_json, config_path);
    } catch (const std::exception &err) {
      std::cerr << "Warning: failed to write default config: " << err.what()
                << "\n";
    }
    return default_config;
  }

  nlohmann::json json_doc;
  try {
    input >> json_doc;
  } catch (const nlohmann::json::parse_error &err) {
    throw std::runtime_error(std::string("Failed to parse config file: ") +
                             err.what());
  }

  out_json = json_doc;

  ClientConfig config;
  auto server_it = json_doc.find("server");
  if (server_it != json_doc.end() && server_it->is_object()) {
    const auto &server_obj = *server_it;
    if (auto ip_it = server_obj.find("serverIP");
        ip_it != server_obj.end() && ip_it->is_string()) {
      config.server_ip = ip_it->get<std::string>();
    }
    if (auto cameras_it = server_obj.find("cameras");
        cameras_it != server_obj.end()) {
      const auto &cameras_node = *cameras_it;
      if (cameras_node.is_array()) {
        for (const auto &camera_entry : cameras_node) {
          const nlohmann::json *camera_obj = nullptr;
          if (camera_entry.is_object()) {
            if (auto nested = camera_entry.find("camera");
                nested != camera_entry.end() && nested->is_object()) {
              camera_obj = &(*nested);
            } else {
              camera_obj = &camera_entry;
            }
          }
          if (!camera_obj) {
            continue;
          }
          CameraConfig camera;
          if (auto name_it = camera_obj->find("name");
              name_it != camera_obj->end() && name_it->is_string()) {
            camera.name = name_it->get<std::string>();
          }
          if (auto ip_it = camera_obj->find("ip");
              ip_it != camera_obj->end() && ip_it->is_string()) {
            camera.ip = ip_it->get<std::string>();
          }
          if (auto via_it = camera_obj->find("viaServer");
              via_it != camera_obj->end() && via_it->is_boolean()) {
            camera.via_server = via_it->get<bool>();
          }
          if (auto orig_it = camera_obj->find("originalUri");
              orig_it != camera_obj->end() && orig_it->is_string()) {
            camera.original_uri = orig_it->get<std::string>();
          }

          const nlohmann::json *options_obj = nullptr;
          if (auto options_it = camera_obj->find("options");
              options_it != camera_obj->end() && options_it->is_object()) {
            options_obj = &(*options_it);
          }

          const auto &option_source = options_obj ? *options_obj : *camera_obj;

          auto assign_bool = [&](const char *key, bool &target) {
            if (auto it = option_source.find(key);
                it != option_source.end() && it->is_boolean()) {
              target = it->get<bool>();
            }
          };
          auto assign_int = [&](const char *key, int &target) {
            if (auto it = option_source.find(key);
                it != option_source.end() && it->is_number_integer()) {
              target = it->get<int>();
            }
          };
          auto assign_float = [&](const char *key, float &target) {
            if (auto it = option_source.find(key);
                it != option_source.end() && it->is_number()) {
              target = static_cast<float>(it->get<double>());
            }
          };
          auto assign_string = [&](const char *key, std::string &target) {
            if (auto it = option_source.find(key);
                it != option_source.end() && it->is_string()) {
              target = it->get<std::string>();
            }
          };

          assign_bool("segment", camera.segment);
          assign_bool("recording", camera.recording);
          assign_bool("overlay", camera.overlay);
          assign_bool("motion_frame", camera.motion_frame);
          assign_bool("gstreamerEncodedProxy", camera.gstreamer_proxy);
          assign_bool("live555proxied", camera.live555_proxy);
          assign_int("segment_bitrate", camera.segment_bitrate);
          assign_string("segment_speed_preset", camera.segment_speed_preset);
          assign_int("proxy_bitrate", camera.proxy_bitrate);
          assign_string("proxy_speed_preset", camera.proxy_speed_preset);
          assign_int("motion_frame_w", camera.motion_frame_width);
          assign_int("motion_frame_h", camera.motion_frame_height);
          assign_float("motion_frame_scale", camera.motion_frame_scale);
          assign_float("noise_threshold", camera.noise_threshold);
          assign_float("motion_threshold", camera.motion_threshold);
          assign_int("motion_min_hits", camera.motion_min_hits);
          assign_int("motion_decay", camera.motion_decay);
          assign_float("motion_arrow_scale", camera.motion_arrow_scale);
          assign_int("motion_arrow_thickness", camera.motion_arrow_thickness);

          // Load RTSP settings (with defaults)
          assign_string("rtsp_transport", camera.rtsp_transport);
          assign_int("rtsp_timeout_seconds", camera.rtsp_timeout_seconds);
          assign_int("max_delay_ms", camera.max_delay_ms);
          assign_int("buffer_size_kb", camera.buffer_size_kb);
          assign_bool("rtsp_flags_prefer_tcp", camera.rtsp_flags_prefer_tcp);
          assign_bool("fflags_nobuffer", camera.fflags_nobuffer);
          assign_int("probesize_kb", camera.probesize_kb);
          assign_int("analyzeduration_ms", camera.analyzeduration_ms);
          assign_bool("low_latency", camera.low_latency);
          assign_int("thread_count", camera.thread_count);
          assign_string("hwaccel", camera.hwaccel);
          assign_bool("limit_frame_rate", camera.limit_frame_rate);

          if (camera.via_server && camera.original_uri.empty()) {
            camera.original_uri = camera.ip;
          }
          if (!camera.ip.empty()) {
            if (camera.name.empty()) {
              camera.name = kUnknownCameraName;
            }
            config.cameras.push_back(std::move(camera));
          }
        }
      }
    }
  }

  auto configuration_it = json_doc.find("configuration");
  if (configuration_it != json_doc.end() && configuration_it->is_object()) {
    const auto &configuration_obj = *configuration_it;
    if (auto general_it = configuration_obj.find("general");
        general_it != configuration_obj.end() && general_it->is_object()) {
      const auto &general_obj = *general_it;
      if (auto width_it = general_obj.find("windowWidth");
          width_it != general_obj.end() && width_it->is_number()) {
        config.window_settings.width =
            static_cast<float>(width_it->get<double>());
      }
      if (auto height_it = general_obj.find("windowHeight");
          height_it != general_obj.end() && height_it->is_number()) {
        config.window_settings.height =
            static_cast<float>(height_it->get<double>());
      }
      if (auto metrics_it = general_obj.find("showImGuiMetrics");
          metrics_it != general_obj.end() && metrics_it->is_boolean()) {
        config.window_settings.show_imgui_metrics = metrics_it->get<bool>();
      }
      if (auto endpoint_it = general_obj.find("serverEndpoint");
          endpoint_it != general_obj.end() && endpoint_it->is_string()) {
        config.server_endpoint = endpoint_it->get<std::string>();
      }
    }
  }

  if (config.server_endpoint.empty()) {
    if (!config.server_ip.empty()) {
      config.server_endpoint = "http://" + config.server_ip + ":8080";
    } else {
      config.server_endpoint = "http://localhost:8080";
    }
  }

  return config;
}

void save_client_config(const nlohmann::json &json_doc,
                        const std::filesystem::path &config_path) {
  std::error_code ec;
  std::filesystem::path parent = config_path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error("Unable to create config directory: " +
                               parent.string() + " (" + ec.message() + ")");
    }
  }

  std::ofstream output(config_path);
  if (!output) {
    throw std::runtime_error("Unable to write config file: " +
                             config_path.string());
  }
  output << std::setw(2) << json_doc << '\n';
}

void sync_json_from_client_config(nlohmann::json &json_doc,
                                  const ClientConfig &config) {
  if (!json_doc.is_object()) {
    json_doc = nlohmann::json::object();
  }

  auto &configuration_node = json_doc["configuration"];
  if (!configuration_node.is_object()) {
    configuration_node = nlohmann::json::object();
  }
  auto &general_node = configuration_node["general"];
  general_node["windowWidth"] = config.window_settings.width;
  general_node["windowHeight"] = config.window_settings.height;
  general_node["showImGuiMetrics"] = config.window_settings.show_imgui_metrics;
  if (!config.server_endpoint.empty()) {
    general_node["serverEndpoint"] = config.server_endpoint;
  }

  auto &server_node = json_doc["server"];
  if (!server_node.is_object()) {
    server_node = nlohmann::json::object();
  }
  if (!config.server_ip.empty()) {
    server_node["serverIP"] = config.server_ip;
  }

  auto &cameras_node = server_node["cameras"];
  cameras_node = nlohmann::json::array();
  for (const auto &camera : config.cameras) {
    if (camera.ip.empty()) {
      continue;
    }

    nlohmann::json camera_json;
    if (!camera.name.empty()) {
      camera_json["name"] = camera.name;
    }
    camera_json["ip"] = camera.ip;

    if (camera.via_server) {
      camera_json["viaServer"] = true;
      if (!camera.original_uri.empty()) {
        camera_json["originalUri"] = camera.original_uri;
      }

      nlohmann::json options = nlohmann::json::object();
      options["segment"] = camera.segment;
      options["recording"] = camera.recording;
      options["overlay"] = camera.overlay;
      options["motion_frame"] = camera.motion_frame;
      options["gstreamerEncodedProxy"] = camera.gstreamer_proxy;
      options["live555proxied"] = camera.live555_proxy;
      if (camera.segment_bitrate > 0) {
        options["segment_bitrate"] = camera.segment_bitrate;
      }
      if (!camera.segment_speed_preset.empty()) {
        options["segment_speed_preset"] = camera.segment_speed_preset;
      }
      if (camera.proxy_bitrate > 0) {
        options["proxy_bitrate"] = camera.proxy_bitrate;
      }
      if (!camera.proxy_speed_preset.empty()) {
        options["proxy_speed_preset"] = camera.proxy_speed_preset;
      }
      if (camera.motion_frame_width > 0) {
        options["motion_frame_w"] = camera.motion_frame_width;
      }
      if (camera.motion_frame_height > 0) {
        options["motion_frame_h"] = camera.motion_frame_height;
      }
      if (camera.motion_frame_scale > 0.0f) {
        options["motion_frame_scale"] = camera.motion_frame_scale;
      }
      if (camera.noise_threshold > 0.0f) {
        options["noise_threshold"] = camera.noise_threshold;
      }
      if (camera.motion_threshold > 0.0f) {
        options["motion_threshold"] = camera.motion_threshold;
      }
      if (camera.motion_min_hits > 0) {
        options["motion_min_hits"] = camera.motion_min_hits;
      }
      if (camera.motion_decay > 0) {
        options["motion_decay"] = camera.motion_decay;
      }
      if (camera.motion_arrow_scale > 0.0f) {
        options["motion_arrow_scale"] = camera.motion_arrow_scale;
      }
      if (camera.motion_arrow_thickness > 0) {
        options["motion_arrow_thickness"] = camera.motion_arrow_thickness;
      }

      // Save RTSP settings (only if non-default)
      if (camera.rtsp_transport != "tcp") {
        options["rtsp_transport"] = camera.rtsp_transport;
      }
      if (camera.rtsp_timeout_seconds != 5) {
        options["rtsp_timeout_seconds"] = camera.rtsp_timeout_seconds;
      }
      if (camera.max_delay_ms != 500) {
        options["max_delay_ms"] = camera.max_delay_ms;
      }
      if (camera.buffer_size_kb != 1024) {
        options["buffer_size_kb"] = camera.buffer_size_kb;
      }
      if (!camera.rtsp_flags_prefer_tcp) {
        options["rtsp_flags_prefer_tcp"] = camera.rtsp_flags_prefer_tcp;
      }
      if (!camera.fflags_nobuffer) {
        options["fflags_nobuffer"] = camera.fflags_nobuffer;
      }
      if (camera.probesize_kb != 1000) {
        options["probesize_kb"] = camera.probesize_kb;
      }
      if (camera.analyzeduration_ms != 1000) {
        options["analyzeduration_ms"] = camera.analyzeduration_ms;
      }
      if (camera.low_latency) {
        options["low_latency"] = camera.low_latency;
      }
      if (camera.thread_count != 0) {
        options["thread_count"] = camera.thread_count;
      }
      if (!camera.hwaccel.empty()) {
        options["hwaccel"] = camera.hwaccel;
      }
      if (!camera.limit_frame_rate) {
        options["limit_frame_rate"] = camera.limit_frame_rate;
      }

      camera_json["options"] = std::move(options);
    }

    cameras_node.push_back(std::move(camera_json));
  }
}

} // namespace client_config
