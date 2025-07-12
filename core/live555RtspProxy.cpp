#include "live555RtspProxy.h"
#include <BasicUsageEnvironment.hh> // OK to keep
#include <UsageEnvironment.hh>

#include <GroupsockHelper.hh> // for port types
#include <iostream>

bool live555RtspProxy::start(uint16_t port) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_)
    return true; // already running is fine

  // Apply output packet buffer sizing before constructing sessions
  OutPacketBuffer::maxSize = outPacketBufferBytes_;

  scheduler_ = BasicTaskScheduler::createNew();
  if (!scheduler_) {
    std::cerr << "LIVE555: Failed to create TaskScheduler\n";
    return false;
  }

  env_ = BasicUsageEnvironment::createNew(*scheduler_);
  if (!env_) {
    std::cerr << "LIVE555: Failed to create UsageEnvironment\n";
    delete scheduler_;
    scheduler_ = nullptr;
    return false;
  }

  server_ = RTSPServer::createNew(*env_, port, nullptr); // no auth DB
  if (!server_) {
    std::cerr << "LIVE555: RTSP server error: " << env_->getResultMsg() << "\n";
    env_->reclaim();
    env_ = nullptr;
    delete scheduler_;
    scheduler_ = nullptr;
    return false;
  }

  // Optional: RTSP-over-HTTP tunneling (for clients)
  if (tryRtspOverHttp_) {
    if (!(server_->setUpTunnelingOverHTTP(80) ||
          server_->setUpTunnelingOverHTTP(8000) ||
          server_->setUpTunnelingOverHTTP(8080))) {
      *env_ << "(RTSP-over-HTTP tunneling not available)\n";
    } else {
      *env_ << "(RTSP-over-HTTP on port " << server_->httpServerPortNum()
            << ")\n";
    }
  }

  port_ = port;
  // eventLoopWatch_ = 0;
  running_ = true;

  std::cout << "Launching Live555 rtsp server!" << std::endl;

  // Run LIVE555 event loop in background
  loopThread_ = std::thread(&live555RtspProxy::eventLoopThread_, this);
  return true;
}

void live555RtspProxy::stop() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!running_)
      return;
    running_ = false; // prevent reentry
    eventLoopWatch_.store(1);
  }

  if (loopThread_.joinable())
    loopThread_.join();

  // Clear our map first so no stale SMS pointers remain
  {
    std::lock_guard<std::mutex> lk(mutex_);
    sessions_.clear();
  }

  if (server_) {
    Medium::close(server_); // This deletes all SMS/clients
    server_ = nullptr;
  }
  if (env_) {
    env_->reclaim();
    env_ = nullptr;
  }
  if (scheduler_) {
    delete scheduler_;
    scheduler_ = nullptr;
  }

  eventLoopWatch_ = 0;

  std::cout << "Live555 rtsp server was stopped." << std::endl;
}

ProxyServerMediaSession *
live555RtspProxy::createProxySession_(const std::string &srcUrl,
                                      const std::string &streamName,
                                      bool forceBackendTCP) {

  // Force RTP-over-TCP for the *backend camera(s)* using the special flag:
  //   tunnelOverHTTPPortNum = ~0  => force TCP (but not HTTP tunneling)
  portNumBits tunnelOverHTTPPortNum = 0;
  if (forceBackendTCP) {
    tunnelOverHTTPPortNum = static_cast<portNumBits>(~0);
  }

  ProxyServerMediaSession *sms = ProxyServerMediaSession::createNew(
      *env_, server_, srcUrl.c_str(), streamName.c_str(),
      /*username*/ nullptr,
      /*password*/ nullptr, tunnelOverHTTPPortNum, verbosityLevel_);

  if (!sms) {
    *env_ << "Failed to create ProxyServerMediaSession for " << srcUrl.c_str()
          << ": " << env_->getResultMsg() << "\n";
  }
  return sms;
}

bool live555RtspProxy::addStream(const std::string &srcUrl,
                                 const std::string &streamName,
                                 bool forceBackendTCP) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_ || !env_ || !server_) {
    std::cerr << "Proxy not running; call start() first\n";
    return false;
  }

  // prevent duplicate names
  if (sessions_.count(streamName)) {
    std::cerr << "Stream name already exists: " << streamName << "\n";
    return false;
  }

  ProxyServerMediaSession *sms =
      createProxySession_(srcUrl, streamName, forceBackendTCP);
  if (!sms)
    return false;

  server_->addServerMediaSession(sms);

  // Keep a handle so we can remove/close later
  sessions_[streamName] = sms;

  // Log URL
  char *url = server_->rtspURL(sms);
  *env_ << "Added proxy: \"" << srcUrl.c_str() << "\" at: " << url << "\n";
  delete[] url;

  return true;
}

std::string live555RtspProxy::streamUrl(const std::string &streamName) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!server_ || !env_)
    return {};

  auto it = sessions_.find(streamName);
  if (it == sessions_.end())
    return {};

  char *url = server_->rtspURL(it->second);
  std::string s = url ? url : "";
  delete[] url;
  return s;
}

void live555RtspProxy::eventLoopThread_() {
  // Note: doEventLoop() will poll until eventLoopWatch_ != 0
  // The char* signature is expected; passing address of our watch var.
  if (!env_)
    return;
  env_->taskScheduler().doEventLoop(&eventLoopWatch_);
}

// removeStream(): schedule deletion, pass the raw pointer
bool live555RtspProxy::removeStream(const std::string &name) {
  if (!server_ || !env_)
    return false;

  ServerMediaSession *sms = nullptr;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sessions_.find(name);
    if (it == sessions_.end())
      return false;
    sms = it->second;
    sessions_.erase(it); // ensure we don’t try twice
  }

  auto *a = new RemoveArgs{this, sms, name};
  env_->taskScheduler().scheduleDelayedTask(
      0, &live555RtspProxy::removeStreamTask, a);
  return true;
}

void live555RtspProxy::removeStreamTask(void *clientData) {
  std::unique_ptr<RemoveArgs> a(static_cast<RemoveArgs *>(clientData));
  auto *self = a->self;
  auto *sms = a->sms;
  if (!self || !self->server_ || !sms)
    return;

  // (optional) kick clients first
  self->server_->closeAllClientSessionsForServerMediaSession(sms);

  // The correct API: tells the server it’s being deleted and frees it.
  self->server_->deleteServerMediaSession(sms);

  if (self->env_) {
    *self->env_ << "Deleted proxy stream '" << a->name.c_str() << "'\n";
  }
}