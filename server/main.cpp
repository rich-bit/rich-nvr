#include "CameraManager.h"
#include "Settings.h"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <opencv2/core/types.hpp>
#include <signal.h>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;
using nlohmann::json;

// Helper function to translate localhost to host.docker.internal when running
// in Docker
std::string translateDockerUri(const std::string &uri) {
  // Check if running in Docker by looking for DOCKER_CONTAINER env var
  const char *docker_env = std::getenv("DOCKER_CONTAINER");
  if (docker_env && std::string(docker_env) == "true") {
    // Replace localhost with host.docker.internal
    std::string result = uri;
    size_t pos = 0;
    while ((pos = result.find("://localhost:", pos)) != std::string::npos) {
      result.replace(pos, 13, "://host.docker.internal:");
      pos += 24; // Length of "://host.docker.internal:"
    }
    // Also handle rtsp://localhost/ (without port)
    pos = 0;
    while ((pos = result.find("://localhost/", pos)) != std::string::npos) {
      result.replace(pos, 13, "://host.docker.internal/");
      pos += 24;
    }
    return result;
  }
  return uri;
}

// Global shutdown flag for signal handling
std::atomic<bool> *g_shutdownRequested = nullptr;

void signalHandler(int signal) {
  std::cout << "\n[Server] Received signal " << signal
            << ", initiating graceful shutdown...\n";
  if (g_shutdownRequested) {
    g_shutdownRequested->store(true);
  }
}

int main() {
  // Local shutdown flag
  std::atomic<bool> shutdownRequested{false};
  g_shutdownRequested = &shutdownRequested;
  // Register signal handlers for graceful shutdown
  signal(SIGINT, signalHandler);  // Ctrl+C
  signal(SIGTERM, signalHandler); // Termination request
  signal(SIGTSTP,
         signalHandler); // Ctrl+Z (suspend signal -> graceful shutdown)
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN); // Ignore broken pipe signals
#endif

  Settings settings("settings.json");

  CameraManager manager(settings);

  httplib::Server svr;

  // HTTP request logging toggle (default OFF)
  std::atomic<bool> enableHttpLogging{false};

  const auto start_time = Clock::now();

  // ---------- Conditional request logger ----------
  svr.set_logger([&enableHttpLogging](const httplib::Request &req,
                                      const httplib::Response &res) {
    if (enableHttpLogging.load()) {
      std::cout << "[HTTP] " << req.method << " " << req.path << " -> "
                << res.status << "\n";
    }
  });

  // Health endpoint
  svr.Get("/health", [&](const httplib::Request &, httplib::Response &res) {
    json j;
    j["ok"] = true;
    j["http_port"] = 8080;
    j["rtsp_proxy_port"] = settings.live_rtsp_proxy_port();
    j["camera_count"] = manager.getCameraNames().size();
    j["uptime_s"] = std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::now() - start_time)
                        .count();
    res.set_content(j.dump(), "application/json");
  });

  // Toggle HTTP logging
  svr.Post("/toggle_logging", [&enableHttpLogging](const httplib::Request &req,
                                                   httplib::Response &res) {
    // Check if enable/disable parameter is provided
    auto action = req.get_param_value("action");

    if (action == "on" || action == "enable" || action == "true" ||
        action == "1") {
      enableHttpLogging.store(true);
    } else if (action == "off" || action == "disable" || action == "false" ||
               action == "0") {
      enableHttpLogging.store(false);
    } else {
      // Toggle current state if no specific action
      enableHttpLogging.store(!enableHttpLogging.load());
    }

    json j;
    j["success"] = true;
    j["http_logging_enabled"] = enableHttpLogging.load();
    j["message"] = enableHttpLogging.load() ? "HTTP logging enabled"
                                            : "HTTP logging disabled";
    res.set_content(j.dump(), "application/json");
  });

  // Shutdown endpoint
  svr.Post("/shutdown", [&shutdownRequested](const httplib::Request &req,
                                             httplib::Response &res) {
    json j;
    j["success"] = true;
    j["message"] = "Server shutdown initiated";
    res.set_content(j.dump(), "application/json");

    // Trigger shutdown after response is sent
    std::thread([&shutdownRequested]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      shutdownRequested.store(true);
    }).detach();
  });

  // List cameras (JSON, detailed)
  svr.Get("/get_cameras",
          [&](const httplib::Request &, httplib::Response &res) {
            auto j = manager.getCamerasInfoJson();
            res.set_content(j.dump(2), "application/json");
          });

  // Add camera
  svr.Post("/add_camera", [&](const httplib::Request &req,
                              httplib::Response &res) {
    auto name = req.get_param_value("name");
    auto uri = req.get_param_value("uri");

    // Translate localhost to host.docker.internal when running in Docker
    uri = translateDockerUri(uri);

    auto param_to_bool = [&](const std::string &key) {
      if (!req.has_param(key))
        return false;
      auto val = req.get_param_value(key);
      return val == "1" || val == "true" || val == "on";
    };

    bool segment = param_to_bool("segment");
    bool recording = param_to_bool("recording");
    bool overlay = param_to_bool("overlay");
    bool motion_frame = param_to_bool("motion_frame");
    bool gstreamerEncodedProxy = param_to_bool("gstreamerEncodedProxy");
    bool live555proxied =
        param_to_bool("live555proxied") || param_to_bool("live555proxy");

    // Segment (recording) settings
    int segment_bitrate =
        req.has_param("segment_bitrate")
            ? std::stoi(req.get_param_value("segment_bitrate"))
            : settings.segment_bitrate();

    std::string segment_speed_preset =
        req.has_param("segment_speed_preset")
            ? req.get_param_value("segment_speed_preset")
            : settings.segment_speedpreset();

    // Proxy (RTSP relay) settings
    int proxy_bitrate = req.has_param("proxy_bitrate")
                            ? std::stoi(req.get_param_value("proxy_bitrate"))
                            : settings.proxy_bitrate();

    std::string proxy_speed_preset =
        req.has_param("proxy_speed_preset")
            ? req.get_param_value("proxy_speed_preset")
            : settings.proxy_speedpreset();

    // Motion frame size (parse as before)
    cv::Size motion_frame_size(0, 0);
    if (req.has_param("motion_frame_size")) {
      std::string sz = req.get_param_value("motion_frame_size");
      size_t delim = sz.find_first_of("x,:");
      if (delim != std::string::npos) {
        int w = std::stoi(sz.substr(0, delim));
        int h = std::stoi(sz.substr(delim + 1));
        motion_frame_size = cv::Size(w, h);
      }
    } else if (req.has_param("motion_frame_w") &&
               req.has_param("motion_frame_h")) {
      int w = std::stoi(req.get_param_value("motion_frame_w"));
      int h = std::stoi(req.get_param_value("motion_frame_h"));
      motion_frame_size = cv::Size(w, h);
    } else {
      IntSize s = settings.motionFrameSize();
      motion_frame_size = cv::Size(s.w, s.h);
    }

    // --- NEW: Motion/Analysis settings ---
    float motion_frame_scale =
        req.has_param("motion_frame_scale")
            ? std::stof(req.get_param_value("motion_frame_scale"))
            : settings.motion_frame_scale();

    float noise_threshold =
        req.has_param("noise_threshold")
            ? std::stof(req.get_param_value("noise_threshold"))
            : settings.noise_threshold();

    float motion_threshold =
        req.has_param("motion_threshold")
            ? std::stof(req.get_param_value("motion_threshold"))
            : settings.motion_threshold();

    int motion_min_hits =
        req.has_param("motion_min_hits")
            ? std::stoi(req.get_param_value("motion_min_hits"))
            : settings.motion_min_hits();

    int motion_decay = req.has_param("motion_decay")
                           ? std::stoi(req.get_param_value("motion_decay"))
                           : settings.motion_decay();

    float motion_arrow_scale =
        req.has_param("motion_arrow_scale")
            ? std::stof(req.get_param_value("motion_arrow_scale"))
            : settings.motion_arrow_scale();

    int motion_arrow_thickness =
        req.has_param("motion_arrow_thickness")
            ? std::stoi(req.get_param_value("motion_arrow_thickness"))
            : settings.motion_arrow_thickness();

    std::string video_output_format =
        req.has_param("video_output_format")
            ? req.get_param_value("video_output_format")
            : settings.video_output_format();

    manager.addCamera(name, uri, segment, recording, overlay, motion_frame,
                      gstreamerEncodedProxy, live555proxied,
                      /*loading=*/false, segment_bitrate, segment_speed_preset,
                      proxy_bitrate, proxy_speed_preset, motion_frame_size,
                      motion_frame_scale, noise_threshold, motion_threshold,
                      motion_min_hits, motion_decay, motion_arrow_scale,
                      motion_arrow_thickness, video_output_format);

    std::ostringstream msg;
    msg << "Camera added (" << "segment=" << segment
        << ", recording=" << recording << ", overlay=" << overlay
        << ", motion_frame=" << motion_frame
        << ", gstreamerEncodedProxy=" << gstreamerEncodedProxy
        << ", live555proxied=" << live555proxied
        << ", segment_bitrate=" << segment_bitrate
        << ", segment_speed_preset=" << segment_speed_preset
        << ", proxy_bitrate=" << proxy_bitrate
        << ", proxy_speed_preset=" << proxy_speed_preset
        << ", motion_frame_size=" << motion_frame_size.width << "x"
        << motion_frame_size.height
        << ", motion_frame_scale=" << motion_frame_scale
        << ", noise_threshold=" << noise_threshold
        << ", motion_threshold=" << motion_threshold
        << ", motion_min_hits=" << motion_min_hits
        << ", motion_decay=" << motion_decay
        << ", motion_arrow_scale=" << motion_arrow_scale
        << ", motion_arrow_thickness=" << motion_arrow_thickness
        << ", video_output_format=" << video_output_format << ")\n";
    res.set_content(msg.str(), "text/plain");
  });

  // Return the latest motion frame as JPEG
  svr.Get("/motion_frame",
          [&](const httplib::Request &req, httplib::Response &res) {
            // Example: /motion_frame?name=cam1
            if (!req.has_param("name")) {
              res.status = 400;
              res.set_content("Missing required parameter: name", "text/plain");
              return;
            }
            std::string name = req.get_param_value("name");
            CameraStream *cam = manager.getCamera(name);
            if (!cam) {
              res.status = 404;
              res.set_content("Camera not found", "text/plain");
              return;
            }
            const cv::Mat &frame = cam->getLastMotionFrame();
            if (frame.empty()) {
              res.status = 404;
              res.set_content("No motion frame available", "text/plain");
              return;
            }

            // Encode to JPEG in-memory
            std::vector<uchar> buf;
            if (!cv::imencode(".jpg", frame, buf)) {
              res.status = 500;
              res.set_content("Failed to encode image", "text/plain");
              return;
            }

            // Send as JPEG
            res.set_content(reinterpret_cast<const char *>(buf.data()),
                            buf.size(), "image/jpeg");
          });

  svr.Get("/favicon.ico",
          [](const httplib::Request &req, httplib::Response &res) {
            res.status = 204; // No Content
          });

  // Remove camera
  svr.Post("/remove_camera",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             manager.removeCamera(name);
             res.set_content("Camera removed\n", "text/plain");
           });

  // Toggle record-on-motion
  svr.Post("/toggle_motion",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             auto value = req.get_param_value("value"); // "on" or "off"
             if (auto cam = manager.getCamera(name)) {
               if (value == "on") {
                 cam->enableMotionFrameSaving(
                     "motion"); // or provide file/path if needed
                 manager.saveSingleCameraToJSON(manager.config_path_, name);
                 res.set_content("Motion recording ON\n", "text/plain");
               } else if (value == "off") {
                 cam->disableMotionFrameSaving();
                 manager.saveSingleCameraToJSON(manager.config_path_, name);
                 res.set_content("Motion recording OFF\n", "text/plain");
               } else {
                 res.status = 400;
                 res.set_content("Invalid value\n", "text/plain");
               }
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });

  // Enable/disable full recording
  svr.Post("/record_on",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             auto file = req.get_param_value("file");
             if (auto cam = manager.getCamera(name)) {
               cam->enableFullRecording(file);
               manager.saveSingleCameraToJSON(manager.config_path_, name);
               res.set_content("Full recording ON\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });
  svr.Post("/record_off",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             if (auto cam = manager.getCamera(name)) {
               cam->disableFullRecording();
               manager.saveSingleCameraToJSON(manager.config_path_, name);
               res.set_content("Full recording OFF\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });

  // Enable/disable overlay
  svr.Post("/overlay_on",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             if (auto cam = manager.getCamera(name)) {
               cam->enableTimestampOverlay();
               res.set_content("Overlay ON\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });
  svr.Post("/overlay_off",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             if (auto cam = manager.getCamera(name)) {
               cam->disableTimestampOverlay();
               res.set_content("Overlay OFF\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });

  // Enable/disable motion frame saving
  svr.Post("/motion_on",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             auto path = req.get_param_value("path");
             if (auto cam = manager.getCamera(name)) {
               cam->enableMotionFrameSaving(path);
               res.set_content("Motion frame saving ON\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });
  svr.Post("/motion_off",
           [&](const httplib::Request &req, httplib::Response &res) {
             auto name = req.get_param_value("name");
             if (auto cam = manager.getCamera(name)) {
               cam->disableMotionFrameSaving();
               res.set_content("Motion frame saving OFF\n", "text/plain");
             } else {
               res.status = 404;
               res.set_content("Camera not found\n", "text/plain");
             }
           });

  // Add motion region to camera
  svr.Post("/add_motion_region", [&](const httplib::Request &req,
                                     httplib::Response &res) {
    auto name = req.get_param_value("name");
    auto x_str = req.get_param_value("x");
    auto y_str = req.get_param_value("y");
    auto w_str = req.get_param_value("w");
    auto h_str = req.get_param_value("h");
    auto angle_str = req.get_param_value("angle"); // Optional parameter

    if (name.empty() || x_str.empty() || y_str.empty() || w_str.empty() ||
        h_str.empty()) {
      res.status = 400;
      res.set_content("Missing required parameters: name, x, y, w, h\n",
                      "text/plain");
      return;
    }

    try {
      int x = std::stoi(x_str);
      int y = std::stoi(y_str);
      int w = std::stoi(w_str);
      int h = std::stoi(h_str);
      float angle = angle_str.empty() ? 0.0f : std::stof(angle_str);

      cv::Rect region(x, y, w, h);
      int regionId = manager.addMotionRegionToCamera(name, region, angle);

      if (regionId != -1) {
        manager.saveSingleCameraToJSON(manager.config_path_, name);
        json response;
        response["success"] = true;
        response["region_id"] = regionId;
        response["angle"] = angle;
        response["message"] = "Motion region added successfully";
        res.set_content(response.dump(), "application/json");
      } else {
        res.status = 404;
        res.set_content("Camera not found\n", "text/plain");
      }
    } catch (const std::exception &e) {
      res.status = 400;
      res.set_content("Invalid numeric parameters\n", "text/plain");
    }
  });

  // Remove motion region from camera
  svr.Post("/remove_motion_region", [&](const httplib::Request &req,
                                        httplib::Response &res) {
    auto name = req.get_param_value("name");
    auto region_id_str = req.get_param_value("region_id");

    if (name.empty() || region_id_str.empty()) {
      res.status = 400;
      res.set_content("Missing required parameters: name, region_id\n",
                      "text/plain");
      return;
    }

    try {
      int regionId = std::stoi(region_id_str);
      bool success = manager.removeMotionRegionFromCamera(name, regionId);

      if (success) {
        manager.saveSingleCameraToJSON(manager.config_path_, name);
        json response;
        response["success"] = true;
        response["message"] = "Motion region removed successfully";
        res.set_content(response.dump(), "application/json");
      } else {
        res.status = 404;
        res.set_content("Camera or region not found\n", "text/plain");
      }
    } catch (const std::exception &e) {
      res.status = 400;
      res.set_content("Invalid region_id parameter\n", "text/plain");
    }
  });

  // Clear all motion regions from camera
  svr.Post("/clear_motion_regions", [&](const httplib::Request &req,
                                        httplib::Response &res) {
    auto name = req.get_param_value("name");

    if (name.empty()) {
      res.status = 400;
      res.set_content("Missing required parameter: name\n", "text/plain");
      return;
    }

    manager.clearMotionRegionsFromCamera(name);
    manager.saveSingleCameraToJSON(manager.config_path_, name);
    json response;
    response["success"] = true;
    response["message"] = "All motion regions cleared successfully";
    res.set_content(response.dump(), "application/json");
  });

  // Get all motion regions from camera
  svr.Get("/get_motion_regions", [&](const httplib::Request &req,
                                     httplib::Response &res) {
    auto name = req.get_param_value("name");

    if (name.empty()) {
      res.status = 400;
      res.set_content("Missing required parameter: name\n", "text/plain");
      return;
    }

    auto regions = manager.getMotionRegionsFromCamera(name);

    json response;
    response["success"] = true;
    response["camera_name"] = name;
    response["regions"] = json::array();

    for (const auto &region : regions) {
      json region_json;
      region_json["id"] = region.id;
      region_json["name"] = "Region " + std::to_string(region.id);
      region_json["x"] = region.rect.x;
      region_json["y"] = region.rect.y;
      region_json["w"] = region.rect.width;
      region_json["h"] = region.rect.height;
      region_json["angle"] = region.angle;
      response["regions"].push_back(region_json);
    }

    res.set_content(response.dump(), "application/json");
  });

  // Update camera properties
  svr.Post("/update_camera_properties", [&](const httplib::Request &req,
                                            httplib::Response &res) {
    auto name = req.get_param_value("name");

    if (name.empty()) {
      res.status = 400;
      res.set_content("Missing required parameter: name\n", "text/plain");
      return;
    }

    auto cam = manager.getCamera(name);
    if (!cam) {
      res.status = 404;
      res.set_content("Camera not found\n", "text/plain");
      return;
    }

    bool updated = false;
    json response;
    response["success"] = true;
    response["camera_name"] = name;
    response["updated_properties"] = json::array();

    // Update motion_frame_scale
    if (req.has_param("motion_frame_scale")) {
      try {
        float value = std::stof(req.get_param_value("motion_frame_scale"));
        cam->setMotionFrameScale(value);
        response["updated_properties"].push_back("motion_frame_scale");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_frame_scale value");
      }
    }

    // Update noise_threshold
    if (req.has_param("noise_threshold")) {
      try {
        float value = std::stof(req.get_param_value("noise_threshold"));
        cam->setNoiseThreshold(value);
        response["updated_properties"].push_back("noise_threshold");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid noise_threshold value");
      }
    }

    // Update motion_threshold
    if (req.has_param("motion_threshold")) {
      try {
        float value = std::stof(req.get_param_value("motion_threshold"));
        cam->setMotionThreshold(value);
        response["updated_properties"].push_back("motion_threshold");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_threshold value");
      }
    }

    // Update motion_min_hits
    if (req.has_param("motion_min_hits")) {
      try {
        int value = std::stoi(req.get_param_value("motion_min_hits"));
        cam->setMotionMinHits(value);
        response["updated_properties"].push_back("motion_min_hits");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_min_hits value");
      }
    }

    // Update motion_decay
    if (req.has_param("motion_decay")) {
      try {
        int value = std::stoi(req.get_param_value("motion_decay"));
        cam->setMotionDecay(value);
        response["updated_properties"].push_back("motion_decay");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_decay value");
      }
    }

    // Update motion_arrow_scale
    if (req.has_param("motion_arrow_scale")) {
      try {
        float value = std::stof(req.get_param_value("motion_arrow_scale"));
        cam->setMotionArrowScale(value);
        response["updated_properties"].push_back("motion_arrow_scale");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_arrow_scale value");
      }
    }

    // Update motion_arrow_thickness
    if (req.has_param("motion_arrow_thickness")) {
      try {
        int value = std::stoi(req.get_param_value("motion_arrow_thickness"));
        cam->setMotionArrowThickness(value);
        response["updated_properties"].push_back("motion_arrow_thickness");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_arrow_thickness value");
      }
    }

    // Update motion_frame_size (width and height)
    if (req.has_param("motion_frame_width") &&
        req.has_param("motion_frame_height")) {
      try {
        int w = std::stoi(req.get_param_value("motion_frame_width"));
        int h = std::stoi(req.get_param_value("motion_frame_height"));
        cam->setMotionFrameSize(cv::Size(w, h));
        response["updated_properties"].push_back("motion_frame_size");
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid motion_frame_size values");
      }
    }

    // Toggle segment recording (record on motion)
    if (req.has_param("segment_recording")) {
      try {
        std::string value = req.get_param_value("segment_recording");
        bool enable = (value == "1" || value == "true" || value == "on");

        if (enable) {
          cam->enableSegmentRecording();
          response["updated_properties"].push_back("segment_recording");
          response["segment_recording"] = true;
        } else {
          cam->disableSegmentRecording();
          response["updated_properties"].push_back("segment_recording");
          response["segment_recording"] = false;
        }
        updated = true;
      } catch (...) {
        response["errors"].push_back("Invalid segment_recording value");
      }
    }

    if (updated) {
      manager.saveSingleCameraToJSON(manager.config_path_, name);
      response["message"] = "Camera properties updated and saved";
    } else {
      response["message"] = "No properties were updated";
    }

    res.set_content(response.dump(), "application/json");
  });

  // Thread info endpoint
  svr.Get("/threads", [&](const httplib::Request &, httplib::Response &res) {
    json threads_array = json::array();

    // Get all camera names
    auto camera_names = manager.getCameraNames();

    // For each camera, add motion thread and segment worker thread
    for (const auto &cam_name : camera_names) {
      auto *cam = manager.getCamera(cam_name);
      if (!cam)
        continue;

      // Motion detection thread
      json motion_thread;
      motion_thread["name"] = "Motion: " + cam_name;
      motion_thread["is_active"] =
          cam->motion_frame(); // Active if motion enabled
      motion_thread["details"] =
          cam->motion_frame() ? "Processing motion frames" : "Disabled";
      threads_array.push_back(motion_thread);

      // Segment worker thread (only if segmentation is enabled)
      if (cam->segment()) {
        json segment_thread;
        segment_thread["name"] = "Segment: " + cam_name;
        segment_thread["is_active"] = true;
        segment_thread["details"] = "Monitoring segment directory";
        threads_array.push_back(segment_thread);
      }
    }

    // GStreamer RTSP proxy thread (if any cameras use it)
    bool has_gst_proxy = false;
    for (const auto &cam_name : camera_names) {
      auto *cam = manager.getCamera(cam_name);
      if (cam && cam->getGstreamerEncodedProxy()) {
        has_gst_proxy = true;
        break;
      }
    }
    if (has_gst_proxy) {
      json gst_thread;
      gst_thread["name"] = "GStreamer RTSP Proxy";
      gst_thread["is_active"] = true;
      gst_thread["details"] = "GLib main loop (port " +
                              std::to_string(settings.live_rtsp_proxy_port()) +
                              ")";
      threads_array.push_back(gst_thread);
    }

    // Live555 RTSP proxy thread (if any cameras use it)
    bool has_live555_proxy = false;
    for (const auto &cam_name : camera_names) {
      auto *cam = manager.getCamera(cam_name);
      if (cam && cam->getLive555Proxied()) {
        has_live555_proxy = true;
        break;
      }
    }
    if (has_live555_proxy) {
      json live555_thread;
      live555_thread["name"] = "Live555 RTSP Proxy";
      live555_thread["is_active"] = true;
      live555_thread["details"] =
          "RTSP server (port " +
          std::to_string(settings.live_rtsp_proxy_port()) + ")";
      threads_array.push_back(live555_thread);
    }

    // HTTP server thread
    json http_thread;
    http_thread["name"] = "HTTP Server";
    http_thread["is_active"] = true;
    http_thread["details"] = "REST API (port 8080)";
    threads_array.push_back(http_thread);

    res.set_content(threads_array.dump(), "application/json");
  });

  svr.set_error_handler(
      [](const httplib::Request &req, httplib::Response &res) {
        std::cout << "[ERROR HANDLER] Path: " << req.path
                  << ", Method: " << req.method << std::endl;
        res.status = 404;
        res.set_content("Custom 404\n", "text/plain");
      });

  std::cout << "HTTP server started on port 8080...\n";

  // Start server in a separate thread to allow for graceful shutdown
  std::thread serverThread([&svr]() { svr.listen("0.0.0.0", 8080); });

  // Wait for shutdown signal
  while (!shutdownRequested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "[Server] Shutting down HTTP server...\n";
  svr.stop();

  std::cout << "[Server] Stopping all cameras and cleaning up...\n";
  manager.stopAll();

  // Wait for server thread to finish
  if (serverThread.joinable()) {
    serverThread.join();
  }

  std::cout << "[Server] Shutdown complete.\n";
  return 0;
}
