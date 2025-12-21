#include "CameraManager.h"
#include "PathUtils.h"
#include "gstreamerRtspProxy.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <thread>
using nlohmann::json;

CameraManager::CameraManager(Settings &settings)
    : settings_(settings), live555_proxy_() {
  gst_init(nullptr, nullptr);

  // Check for CONFIG_PATH environment variable
  const char *env_config_path = std::getenv("CONFIG_PATH");
  if (env_config_path && *env_config_path) {
    config_path_ = env_config_path;
  } else {
    std::string exe_dir = core::PathUtils::getExecutableDir();
    config_path_ = exe_dir + "/cameras.json";
  }

  loadCamerasFromJSON(config_path_);
}

CameraManager::~CameraManager() {
  stopAll();

  gstreamer_proxy_.stop();
  live555_proxy_.stop();

  std::cout << "CameraManager exit after destructor" << std::endl;
}

void CameraManager::addCamera(
    const std::string &name, const std::string &uri, bool segment,
    bool recording, bool overlay, bool motion_frame, bool gstreamerEncodedProxy,
    bool live555proxied, bool loading, int segment_bitrate,
    const std::string &segment_speed_preset, int proxy_bitrate,
    const std::string &proxy_speed_preset, cv::Size motion_frame_size,
    float motion_frame_scale, float noise_threshold, float motion_threshold,
    int motion_min_hits, int motion_decay, float motion_arrow_scale,
    int motion_arrow_thickness, std::string video_output_format,
    std::optional<AudioProbeResult> audio_hint) {
  if (cameras_.find(name) != cameras_.end())
    return;

  if (live555proxied && gstreamerEncodedProxy)
    std::cout << "Dont use live55proxy and gstreamer encodinga at once.";

  std::string csName = name;

  // If Live555 proxying is requested, ensure Live555 is up
  if (live555proxied && !gstreamerEncodedProxy) {
    if (!live555_proxy_.isRunning()) {
      if (!live555_proxy_.start(live555_port_)) {
        std::cerr << "Live555 proxy failed to start on port " << live555_port_
                  << "\n";
      }
    }
    // Sanitize and derive a stream name used in the Live555 server path
    std::string sanitizeCameraName = core::PathUtils::sanitizeCameraName(name);
    std::string streamName = "cam/" + sanitizeCameraName;

    // Add the upstream -> proxy mapping
    if (!live555_proxy_.addStream(uri, streamName, /*forceBackendTCP=*/true)) {
      std::cerr << "Live555 proxy failed to add stream '" << name << "' ("
                << uri << ")\n";
    } else {
      auto url = live555_proxy_.streamUrl(streamName);
      if (!url.empty())
        std::cout << "Live555: " << name << " at " << url << "\n";

      csName = sanitizeCameraName;
    }
  }

  if (gstreamerEncodedProxy && !live555proxied) {
    if (!gstreamer_proxy_.isRunning()) {
      if (!gstreamer_proxy_.start(/*port*/ 8554)) {
        std::cerr << "Failed to start GStreamer RTSP proxy on 8554\n";
      }
    }
    gstreamer_proxy_.addCameraProxy(name,
                                    /*bitrate*/ proxy_bitrate,
                                    /*speed_preset*/ proxy_speed_preset);
  }

  auto cam = std::make_unique<CameraStream>(
      csName, uri, settings_, segment, recording, overlay, motion_frame,
      gstreamerEncodedProxy, live555proxied, proxy_bitrate, proxy_speed_preset,
      segment_bitrate, segment_speed_preset, motion_frame_size,
      motion_frame_scale, noise_threshold, motion_threshold, motion_min_hits,
      motion_decay, motion_arrow_scale, motion_arrow_thickness,
      video_output_format);

  if (audio_hint) {
    cam->setAudioHint(*audio_hint);
  }

  cam->start();
  cameras_[name] = std::move(cam);

  if (gstreamerEncodedProxy && !live555proxied) {
    if (!gstreamer_proxy_.isRunning()) {
      if (!gstreamer_proxy_.start(8554)) {
        std::cerr << "Failed to start GStreamer RTSP proxy\n";
      }
    }
    if (gstreamer_proxy_.isRunning()) {
      gstreamer_proxy_.addCameraProxy(name, proxy_bitrate, proxy_speed_preset);
    }
  }

  if (!loading)
    saveCamerasToJSON(config_path_);
}

void CameraManager::saveCamerasToJSON(const std::string &filename) {
  nlohmann::json j;
  for (const auto &pair : cameras_) {
    const auto &cam = pair.second;
    nlohmann::json cam_json;
    cam_json["name"] = cam->name();
    cam_json["uri"] = cam->uri();
    cam_json["segment"] = cam->segment();
    cam_json["recording"] = cam->recording();
    cam_json["overlay"] = cam->overlay();
    cam_json["motion_frame"] = cam->motion_frame();
    cam_json["gstreamerEncodedProxy"] = cam->getGstreamerEncodedProxy();
    cam_json["live555proxied"] = cam->getLive555Proxied();

    cam_json["segment_bitrate"] = cam->getSegmentBitrate();
    cam_json["segment_speed_preset"] = cam->getSegmentSpeedPreset();
    cam_json["proxy_bitrate"] = cam->getProxyBitrate();
    cam_json["proxy_speed_preset"] = cam->getProxySpeedPreset();

    cam_json["motion_frame_scale"] = cam->getMotionFrameScale();
    cam_json["noise_threshold"] = cam->getNoiseThreshold();
    cam_json["motion_threshold"] = cam->getMotionThreshold();
    cam_json["motion_min_hits"] = cam->getMotionMinHits();
    cam_json["motion_decay"] = cam->getMotionDecay();
    cam_json["motion_arrow_scale"] = cam->getMotionArrowScale();
    cam_json["motion_arrow_thickness"] = cam->getMotionArrowThickness();
    cam_json["video_output_format"] = cam->getVideoOutputFormat();

    cv::Size sz = cam->getMotionFrameSize();
    cam_json["motion_frame_size"] = {sz.width, sz.height};

    const auto &ap = cam->audioProbe();
    cam_json["audio"] = {{"has_audio", ap.has_audio},
                         {"encoding", ap.encoding},
                         {"rate", ap.rate},
                         {"channels", ap.channels}};

    // Save motion regions
    const auto &regions = cam->getMotionRegions();
    cam_json["motion_regions"] = nlohmann::json::array();
    for (const auto &region : regions) {
      nlohmann::json region_json;
      region_json["id"] = region.id;
      region_json["x"] = region.rect.x;
      region_json["y"] = region.rect.y;
      region_json["w"] = region.rect.width;
      region_json["h"] = region.rect.height;
      region_json["angle"] = region.angle;
      cam_json["motion_regions"].push_back(region_json);
    }

    j["cameras"].push_back(cam_json);
  }
  std::ofstream file(filename);
  if (file) {
    file << j.dump(2) << std::endl;
    std::cout << "Cameras saved to " << filename << std::endl;
  } else {
    std::cerr << "Failed to write " << filename << std::endl;
  }
}

void CameraManager::saveSingleCameraToJSON(const std::string &filename,
                                           const std::string &cameraName) {
  // Load existing JSON
  nlohmann::json j;
  std::ifstream inFile(filename);
  if (inFile.is_open()) {
    try {
      inFile >> j;
    } catch (...) {
      std::cerr << "Error reading existing cameras.json, creating new one"
                << std::endl;
      j = nlohmann::json::object();
    }
    inFile.close();
  }

  // Ensure cameras array exists
  if (!j.contains("cameras") || !j["cameras"].is_array()) {
    j["cameras"] = nlohmann::json::array();
  }

  // Find the camera to update
  auto it = cameras_.find(cameraName);
  if (it == cameras_.end()) {
    std::cerr << "Camera '" << cameraName << "' not found for JSON update"
              << std::endl;
    return;
  }

  const auto &cam = it->second;
  nlohmann::json cam_json;
  cam_json["name"] = cam->name();
  cam_json["uri"] = cam->uri();
  cam_json["segment"] = cam->segment();
  cam_json["recording"] = cam->recording();
  cam_json["overlay"] = cam->overlay();
  cam_json["motion_frame"] = cam->motion_frame();
  cam_json["gstreamerEncodedProxy"] = cam->getGstreamerEncodedProxy();
  cam_json["live555proxied"] = cam->getLive555Proxied();

  cam_json["segment_bitrate"] = cam->getSegmentBitrate();
  cam_json["segment_speed_preset"] = cam->getSegmentSpeedPreset();
  cam_json["proxy_bitrate"] = cam->getProxyBitrate();
  cam_json["proxy_speed_preset"] = cam->getProxySpeedPreset();

  cam_json["motion_frame_scale"] = cam->getMotionFrameScale();
  cam_json["noise_threshold"] = cam->getNoiseThreshold();
  cam_json["motion_threshold"] = cam->getMotionThreshold();
  cam_json["motion_min_hits"] = cam->getMotionMinHits();
  cam_json["motion_decay"] = cam->getMotionDecay();
  cam_json["motion_arrow_scale"] = cam->getMotionArrowScale();
  cam_json["motion_arrow_thickness"] = cam->getMotionArrowThickness();
  cam_json["video_output_format"] = cam->getVideoOutputFormat();

  cv::Size sz = cam->getMotionFrameSize();
  cam_json["motion_frame_size"] = {sz.width, sz.height};

  const auto &ap = cam->audioProbe();
  cam_json["audio"] = {{"has_audio", ap.has_audio},
                       {"encoding", ap.encoding},
                       {"rate", ap.rate},
                       {"channels", ap.channels}};

  // Save motion regions
  const auto &regions = cam->getMotionRegions();
  cam_json["motion_regions"] = nlohmann::json::array();
  for (const auto &region : regions) {
    nlohmann::json region_json;
    region_json["id"] = region.id;
    region_json["x"] = region.rect.x;
    region_json["y"] = region.rect.y;
    region_json["w"] = region.rect.width;
    region_json["h"] = region.rect.height;
    region_json["angle"] = region.angle;
    cam_json["motion_regions"].push_back(region_json);
  }

  // Find and replace existing camera entry, or add if not found
  bool found = false;
  for (auto &entry : j["cameras"]) {
    if (entry.is_object() && entry["name"] == cameraName) {
      entry = cam_json;
      found = true;
      break;
    }
  }

  if (!found) {
    j["cameras"].push_back(cam_json);
  }

  // Write back to file
  std::ofstream outFile(filename);
  if (outFile) {
    outFile << j.dump(2) << std::endl;
    std::cout << "Camera '" << cameraName << "' saved to " << filename
              << std::endl;
  } else {
    std::cerr << "Failed to write " << filename << std::endl;
  }
}

void CameraManager::loadCamerasFromJSON(const std::string &filename) {
  std::ifstream f(filename);
  if (!f.is_open()) {
    std::cout << "[CameraManager] cameras.json not found at: " << filename
              << std::endl;
    std::cout << "[CameraManager] Creating new empty cameras.json..."
              << std::endl;

    // Create parent directory if it doesn't exist
    std::filesystem::path file_path(filename);
    if (file_path.has_parent_path()) {
      std::filesystem::create_directories(file_path.parent_path());
    }

    // Create empty cameras array
    nlohmann::json j;
    j["cameras"] = nlohmann::json::array();

    std::ofstream out(filename);
    if (out) {
      out << j.dump(2) << std::endl;
      std::cout << "[CameraManager] Created empty cameras.json at: " << filename
                << std::endl;
    } else {
      std::cerr << "[CameraManager] Warning: Could not create cameras.json at: "
                << filename << std::endl;
    }
    return;
  }

  nlohmann::json j;
  try {
    f >> j;
  } catch (...) {
    std::cerr << "[CameraManager] Error reading/parsing cameras.json!"
              << std::endl;
    return;
  }

  if (j.contains("cameras") && j["cameras"].is_array()) {
    for (const auto &entry : j["cameras"]) {
      std::string name = entry.value("name", "");
      std::string uri = entry.value("uri", "");
      bool segment = entry.value("segment", false);
      bool recording = entry.value("recording", false);
      bool overlay = entry.value("overlay", false);
      bool motion_frame = entry.value("motion_frame", false);
      bool gstreamerEncodedProxy = entry.value("gstreamerEncodedProxy", false);
      bool live555proxied = entry.value("live555proxied", false);

      int segment_bitrate = entry.contains("segment_bitrate")
                                ? entry["segment_bitrate"].get<int>()
                                : settings_.segment_bitrate();
      std::string segment_speed_preset =
          entry.contains("segment_speed_preset")
              ? entry["segment_speed_preset"].get<std::string>()
              : settings_.segment_speedpreset();
      int proxy_bitrate = entry.contains("proxy_bitrate")
                              ? entry["proxy_bitrate"].get<int>()
                              : settings_.proxy_bitrate();
      std::string proxy_speed_preset =
          entry.contains("proxy_speed_preset")
              ? entry["proxy_speed_preset"].get<std::string>()
              : settings_.proxy_speedpreset();

      AudioProbeResult audio_hint;
      bool have_audio_hint = false;
      if (entry.contains("audio") && entry["audio"].is_object()) {
        const auto &a = entry["audio"];
        audio_hint.has_audio = a.value("has_audio", false);
        audio_hint.encoding = a.value("encoding", std::string{});
        audio_hint.rate = a.value("rate", 0);
        audio_hint.channels = a.value("channels", 0);
        audio_hint.probed = true;
        have_audio_hint = true;
      }

      float motion_frame_scale = entry.value("motion_frame_scale", 1.0f);
      float noise_threshold = entry.value("noise_threshold", 0.0f);
      float motion_threshold = entry.value("motion_threshold", 0.0f);
      int motion_min_hits = entry.value("motion_min_hits", 1);
      int motion_decay = entry.value("motion_decay", 0);
      float motion_arrow_scale = entry.value("motion_arrow_scale", 2.5f);
      int motion_arrow_thickness = entry.value("motion_arrow_thickness", 1);
      std::string video_output_format =
          entry.value("video_output_format", "mp4");

      IntSize s = settings_.motionFrameSize();
      cv::Size motion_frame_size(s.w, s.h);

      if (entry.contains("motion_frame_size") &&
          entry["motion_frame_size"].is_array()) {
        auto arr = entry["motion_frame_size"];
        if (arr.size() == 2)
          motion_frame_size = cv::Size(arr[0], arr[1]);
      }

      if (!name.empty() && !uri.empty()) {
        addCamera(name, uri, segment, recording, overlay, motion_frame,
                  gstreamerEncodedProxy, live555proxied,
                  /*loading=*/true, segment_bitrate, segment_speed_preset,
                  proxy_bitrate, proxy_speed_preset, motion_frame_size,
                  // --- New motion-related params:
                  motion_frame_scale, noise_threshold, motion_threshold,
                  motion_min_hits, motion_decay, motion_arrow_scale,
                  motion_arrow_thickness, video_output_format,
                  have_audio_hint ? std::optional<AudioProbeResult>{audio_hint}
                                  : std::nullopt);

        // Load motion regions after camera is added
        if (entry.contains("motion_regions") &&
            entry["motion_regions"].is_array()) {
          for (const auto &region_json : entry["motion_regions"]) {
            int x = region_json.value("x", 0);
            int y = region_json.value("y", 0);
            int w = region_json.value("w", 0);
            int h = region_json.value("h", 0);
            float angle = region_json.value("angle", 0.0f);
            cv::Rect rect(x, y, w, h);
            addMotionRegionToCamera(name, rect, angle);
          }
        }
      }
    }
  } else {
    std::cerr << "Malformed cameras.json: 'cameras' array not found\n";
  }
}

void CameraManager::removeCamera(const std::string &name) {
  // Remove and stop CameraStream
  auto it = cameras_.find(name);
  bool was_gst_proxied = false;
  bool was_live555 = false;

  if (it != cameras_.end()) {
    was_gst_proxied = it->second->getGstreamerEncodedProxy();
    was_live555 = it->second->getLive555Proxied();
    it->second->stop();
    cameras_.erase(it);
  }

  if (was_gst_proxied) {
    gstreamer_proxy_.removeCameraProxy(
        name); // just unmount, server keeps running
  }

  if (was_live555) {
    // If your live555 wrapper has removeStream(streamName):
    std::string streamName = "cam/" + name;
    live555_proxy_.removeStream(streamName); // implement if not present

    if (live555_proxy_.sessionCount() == 0) {
      live555_proxy_
          .stop(); // safe: closes server; any pending SMS deletes are fine
    }
  }

  std::cout << "Closed stream /cam/" << name << std::endl;

  // Optionally: re-save cameras.json
  saveCamerasToJSON(config_path_);
}

CameraStream *CameraManager::getCamera(const std::string &name) {
  auto it = cameras_.find(name);
  if (it != cameras_.end())
    return it->second.get();
  return nullptr;
}

void CameraManager::startAll() {
  for (auto &[_, cam] : cameras_)
    cam->start();
}

void CameraManager::stopAll() {
  for (auto &[_, cam] : cameras_)
    cam->stop();
}

std::vector<std::string> CameraManager::getCameraNames() const {
  std::vector<std::string> names;
  for (const auto &pair : cameras_)
    names.push_back(pair.first);
  return names;
}
nlohmann::json CameraManager::getCamerasInfoJson() const {
  json arr = json::array();

  for (const auto &kv : cameras_) {
    const auto &camPtr = kv.second;
    if (!camPtr)
      continue;
    const CameraStream &cam = *camPtr;

    json j;
    j["name"] = cam.name();
    j["uri"] = cam.uri();
    j["segment"] = cam.segment();
    j["recording"] = cam.recording();
    j["overlay"] = cam.overlay();
    j["motion_frame"] = cam.motion_frame();
    j["gstreamerEncodedProxy"] = cam.getGstreamerEncodedProxy();
    j["live555Proxied"] = cam.getLive555Proxied();

    j["proxy_bitrate"] = cam.getProxyBitrate();
    j["proxy_speed_preset"] = cam.getProxySpeedPreset();
    j["segment_bitrate"] = cam.getSegmentBitrate();
    j["segment_speed_preset"] = cam.getSegmentSpeedPreset();

    const cv::Size msz = cam.getMotionFrameSize();
    j["motion_frame_size"] = {msz.width, msz.height};
    j["motion_frame_scale"] = cam.getMotionFrameScale();

    j["noise_threshold"] = cam.getNoiseThreshold();
    j["motion_threshold"] = cam.getMotionThreshold();
    j["motion_min_hits"] = cam.getMotionMinHits();
    j["motion_decay"] = cam.getMotionDecay();
    j["motion_arrow_scale"] = cam.getMotionArrowScale();
    j["motion_arrow_thickness"] = cam.getMotionArrowThickness();
    j["video_output_format"] = cam.getVideoOutputFormat();

    // Helpful extras
    j["mount_point"] = cam.getMountPoint();
    j["has_motion_frame"] = !cam.getLastMotionFrame().empty();

    // If proxied via Live555, expose the RTSP URL the client can use
    if (cam.getLive555Proxied()) {
      std::string mount = cam.getMountPoint();
      if (!mount.empty() && mount.front() != '/')
        mount.insert(mount.begin(), '/');

      const int port =
          settings_.live_rtsp_proxy_port(); // your configurable port
      const std::string host = live555_bind_host_;

      j["live_proxied_rtsp_path"] = "cam/" + cam.name();
    } else {
      j["live_proxied_rtsp_path"] = nullptr;
    }

    arr.push_back(std::move(j));
  }

  return arr;
}

int CameraManager::addMotionRegionToCamera(const std::string &cameraId,
                                           const cv::Rect &region,
                                           float angle) {
  auto it = cameras_.find(cameraId);
  if (it == cameras_.end()) {
    std::cout << "[CameraManager] Camera '" << cameraId
              << "' not found for motion region" << std::endl;
    return -1;
  }

  int regionId = it->second->addMotionRegion(region, angle);
  std::cout << "[CameraManager] Added motion region " << regionId
            << " to camera '" << cameraId << "' with angle " << angle << "°"
            << std::endl;
  return regionId;
}

bool CameraManager::removeMotionRegionFromCamera(const std::string &cameraId,
                                                 int regionId) {
  auto it = cameras_.find(cameraId);
  if (it == cameras_.end()) {
    std::cout << "[CameraManager] Camera '" << cameraId
              << "' not found for motion region removal" << std::endl;
    return false;
  }

  bool success = it->second->removeMotionRegion(regionId);
  if (success) {
    std::cout << "[CameraManager] Removed motion region " << regionId
              << " from camera '" << cameraId << "'" << std::endl;
  }
  return success;
}

void CameraManager::clearMotionRegionsFromCamera(const std::string &cameraId) {
  auto it = cameras_.find(cameraId);
  if (it == cameras_.end()) {
    std::cout << "[CameraManager] Camera '" << cameraId
              << "' not found for motion region clearing" << std::endl;
    return;
  }

  it->second->clearMotionRegions();
  std::cout << "[CameraManager] Cleared all motion regions from camera '"
            << cameraId << "'" << std::endl;
}

std::vector<MotionRegion>
CameraManager::getMotionRegionsFromCamera(const std::string &cameraId) const {
  auto it = cameras_.find(cameraId);
  if (it == cameras_.end()) {
    std::cout << "[CameraManager] Camera '" << cameraId
              << "' not found for getting motion regions" << std::endl;
    return {};
  }

  return it->second->getMotionRegions();
}