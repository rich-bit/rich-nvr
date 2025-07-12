#pragma once

#include <atomic>
#include <string>
#include <thread>

// GStreamer / RTSP
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

class GstreamerRtspProxy {
public:
  GstreamerRtspProxy();
  ~GstreamerRtspProxy();

  // Start the RTSP server and main loop thread.
  // Returns true on success. Default port 8554.
  bool start(int port = 8554);

  // Add a proxied camera mount using the “intervideosrc … x264enc … rtph264pay”
  // pipeline. Returns true on success.
  bool addCameraProxy(const std::string &camName, int bitrate,
                      const std::string &speedPreset);

  // Stop server and join thread.
  void stop();

  bool isRunning() const { return rtsp_running_.load(); }
  std::string endpoint() const;
  bool removeCameraProxy(const std::string &camName);

private:
  void threadFunc_();

  std::atomic<int> mount_count_{0};

  GMainLoop *main_loop_ = nullptr;
  GstRTSPServer *rtsp_server_ = nullptr;
  GstRTSPMountPoints *mounts_ = nullptr;

  std::thread rtsp_thread_;
  std::atomic<bool> rtsp_running_{false};
  int port_ = 8554;
};
