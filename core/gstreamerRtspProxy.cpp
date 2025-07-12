#include "gstreamerRtspProxy.h"

#include <iostream>

// GStreamer / GLib
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-media-factory.h>
#include <gst/rtsp-server/rtsp-mount-points.h>
#include <gst/rtsp-server/rtsp-server.h>

GstreamerRtspProxy::GstreamerRtspProxy() = default;

GstreamerRtspProxy::~GstreamerRtspProxy() { stop(); }

bool GstreamerRtspProxy::start(int port) {
  // NOTE: gst_init(nullptr, nullptr) is intentionally not called here.
  // Do it once in your app (e.g., CameraManager constructor) before using this
  // class.
  // Or:
  // if (!gst_is_initialized()) { gst_init(nullptr, nullptr); }
  port_ = port;

  if (rtsp_running_.load())
    return true;

  main_loop_ = g_main_loop_new(nullptr, FALSE);
  if (!main_loop_) {
    std::cerr << "GstreamerRtspProxy: Failed to create GLib main loop\n";
    return false;
  }

  rtsp_server_ = gst_rtsp_server_new();
  if (!rtsp_server_) {
    std::cerr << "GstreamerRtspProxy: Failed to create RTSP server\n";
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
    return false;
  }

  gst_rtsp_server_set_service(rtsp_server_, std::to_string(port_).c_str());

  mounts_ = gst_rtsp_server_get_mount_points(rtsp_server_);
  if (!mounts_) {
    std::cerr << "GstreamerRtspProxy: Failed to get RTSP mount points\n";
    g_object_unref(rtsp_server_);
    rtsp_server_ = nullptr;
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
    return false;
  }

  // Attach the server to default main context; keep source id implicit.
  if (gst_rtsp_server_attach(rtsp_server_, nullptr) == 0) {
    std::cerr << "GstreamerRtspProxy: Failed to attach RTSP server\n";
    g_object_unref(mounts_);
    mounts_ = nullptr;
    g_object_unref(rtsp_server_);
    rtsp_server_ = nullptr;
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
    return false;
  }

  std::cout << "RTSP proxy server started at " << endpoint() << std::endl;

  rtsp_running_.store(true);
  rtsp_thread_ = std::thread(&GstreamerRtspProxy::threadFunc_, this);
  return true;
}

bool GstreamerRtspProxy::addCameraProxy(const std::string &camName, int bitrate,
                                        const std::string &speedPreset) {
  if (!mounts_) {
    std::cerr << "GstreamerRtspProxy: addCameraProxy called before start()\n";
    return false;
  }

  // Build pipeline identical to your original one
  // intervideosrc channel=<camName> ! videoconvert ! x264enc tune=zerolatency
  // bitrate=<bitrate> speed-preset=<preset> ! h264parse ! rtph264pay name=pay0
  // pt=96
  std::string mountPoint = "cam/" + camName;
  std::string pipeline = "intervideosrc channel=" + camName +
                         " ! videoconvert ! x264enc tune=zerolatency bitrate=" +
                         std::to_string(bitrate) +
                         " speed-preset=" + speedPreset +
                         " ! h264parse ! rtph264pay name=pay0 pt=96";

  GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
  if (!factory) {
    std::cerr << "GstreamerRtspProxy: Failed to create RTSP media factory\n";
    return false;
  }

  gst_rtsp_media_factory_set_launch(factory, pipeline.c_str());
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_mount_points_add_factory(mounts_, mountPoint.c_str(), factory);

  mount_count_.fetch_add(1, std::memory_order_relaxed);

  std::cout << "[RTSP proxy pipeline] " << pipeline << std::endl;
  std::cout << "Stream proxied at " << endpoint() << "cam/" << camName
            << std::endl;
  return true;
}

void GstreamerRtspProxy::stop() {
  bool wasRunning = rtsp_running_.exchange(false);
  if (!wasRunning)
    return;

  if (main_loop_)
    g_main_loop_quit(main_loop_);
  if (rtsp_thread_.joinable())
    rtsp_thread_.join();

  // Unref in reverse order of acquisition
  if (mounts_) {
    g_object_unref(mounts_);
    mounts_ = nullptr;
  }
  if (rtsp_server_) {
    g_object_unref(rtsp_server_);
    rtsp_server_ = nullptr;
  }
  if (main_loop_) {
    g_main_loop_unref(main_loop_);
    main_loop_ = nullptr;
  }

  std::cout << "RTSP proxy server stopped." << std::endl;
}

void GstreamerRtspProxy::threadFunc_() {
  if (!main_loop_)
    return;
  g_main_loop_run(main_loop_);
  // After g_main_loop_quit(), we land here:
  // Nothing else to do; cleanup handled in stop().
  std::cout << "RTSP server loop exited." << std::endl;
}

std::string GstreamerRtspProxy::endpoint() const {
  // We don’t set address here (defaults to any). Use loopback in message.
  return "rtsp://127.0.0.1:" + std::to_string(port_) + "/";
}
bool GstreamerRtspProxy::removeCameraProxy(const std::string &camName) {
  if (!mounts_) {
    std::cerr
        << "GstreamerRtspProxy: removeCameraProxy called before start()\n";
    return false;
  }
  std::string mountPoint = "cam/" + camName;

  gst_rtsp_mount_points_remove_factory(mounts_, mountPoint.c_str());

  // (Optional) track how many mounts are left, for logs/metrics only
  int left = mount_count_.fetch_sub(1, std::memory_order_relaxed) - 1;
  std::cout << "Removed proxy at " << mountPoint << " (remaining: " << left
            << ")\n";
  return true;
}