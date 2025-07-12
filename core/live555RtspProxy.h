#pragma once

#include <BasicUsageEnvironment.hh>
#include <liveMedia.hh>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

/**
 * Thin wrapper around LIVE555's ProxyServerMediaSession + RTSPServer.
 *
 * Usage:
 *   live555RtspProxy proxy;
 *   proxy.start(8554); // starts event loop on a background thread
 *   proxy.addStream("rtsp://pi4.l:8554/cam", "proxyStream-1");
 *   proxy.addStream("rtsp://z2w.l:8554/cam_with_audio", "proxyStream-2");
 *   ...
 *   proxy.removeStream("proxyStream-1");
 *   proxy.stop();
 */

class live555RtspProxy {
public:
  // You can tweak these via constructor if you want different defaults.
  explicit live555RtspProxy(unsigned outPacketBufferBytes = 600000,
                            int verbosityLevel = 0, bool tryRtspOverHttp = true)
      : outPacketBufferBytes_(outPacketBufferBytes),
        verbosityLevel_(verbosityLevel), tryRtspOverHttp_(tryRtspOverHttp) {}

  ~live555RtspProxy() { stop(); }

  // Start RTSP server and event loop on a background thread.
  // Returns false if server could not be created/bound.
  bool start(uint16_t port = 8554);

  // Stop event loop, remove sessions, free resources.
  void stop();

  // Add a proxied upstream stream immediately.
  // - streamName must be unique (used in RTSP URL path).
  // - Forces RTP-over-TCP to BACKEND by default (matches your working code).
  // Returns false on error (e.g., duplicate name or LIVE555 failure).
  bool addStream(const std::string &srcUrl, const std::string &streamName,
                 bool forceBackendTCP = true);

  // Remove a proxied stream by its name. Safe to call even if not present.
  // Returns true if something was removed.
  bool removeStream(const std::string &streamName);

  // True while event loop thread is running.
  bool isRunning() const { return running_.load(); }
  size_t sessionCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return sessions_.size();
  }

  // Returns the server URL for a stream (rtsp://host:port/name).
  // Returns empty string if not running or name unknown.
  std::string streamUrl(const std::string &streamName) const;

  // Port the server is bound to (valid after start()).
  uint16_t serverPort() const { return port_; }

private:
  static void closeMediumTask(void *clientData);
  // Internal helpers (must be called with mutex_ held where noted).
  ProxyServerMediaSession *createProxySession_(const std::string &srcUrl,
                                               const std::string &streamName,
                                               bool forceBackendTCP);

  void eventLoopThread_();

  struct RemoveArgs {
    live555RtspProxy *self;
    ServerMediaSession *sms;
    std::string name;
  };

  static void removeStreamTask(void *clientData);
  // Configuration
  const unsigned outPacketBufferBytes_;
  const int verbosityLevel_;
  const bool tryRtspOverHttp_;

  // Live555 core
  TaskScheduler *scheduler_ = nullptr;
  UsageEnvironment *env_ = nullptr;
  RTSPServer *server_ = nullptr;

  // Event loop control
  std::thread loopThread_;
  std::atomic<bool> running_{false};
  // LIVE555 uses a watch variable (non-zero => exit loop)
  EventLoopWatchVariable eventLoopWatch_{0};

  // Sessions keyed by stream name
  mutable std::mutex mutex_;
  std::map<std::string, ServerMediaSession *> sessions_;

  // Server bind port
  uint16_t port_ = 0;
};
