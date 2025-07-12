#include "VideoPlayer.h"
#include "ClientSettings.h"
#include "conversations.h"
#include "PathUtils.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QShortcut>
#include <QTimer>
#include <QThread>
#include <QMetaObject>
#include <QString>
#include <QByteArray>
#include <iostream>
#include <QUrl>
#include <QVBoxLayout>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <QGuiApplication>
#include "VideoSurface.h"

#ifdef Q_OS_WIN
#include <gst/video/videooverlay.h>
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <gst/video/videooverlay.h>
#include <X11/Xlib.h>
#endif

static inline bool isX11Session() {
  const auto qpa = qgetenv("QT_QPA_PLATFORM").toLower();
  if (qpa == "xcb") return true;
  const auto plat = QGuiApplication::platformName().toLower();
  if (plat.contains("xcb")) return true;
  const auto sess = qgetenv("XDG_SESSION_TYPE").toLower();
  if (sess == "x11") return true;
  return qEnvironmentVariableIsSet("DISPLAY") && !qEnvironmentVariableIsSet("WAYLAND_DISPLAY");
}


static inline bool isWaylandSession() {
  const auto qpa = qgetenv("QT_QPA_PLATFORM").toLower();
  if (qpa.startsWith("wayland")) return true;
  const auto plat = QGuiApplication::platformName().toLower();
  if (plat.contains("wayland")) return true;
  const auto sess = qgetenv("XDG_SESSION_TYPE").toLower();
  if (sess == "wayland") return true;
  return qEnvironmentVariableIsSet("WAYLAND_DISPLAY") && !isX11Session();
}

VideoPlayer::VideoPlayer(QWidget *parent)
    : QMainWindow(parent), pipeline(nullptr) {
  setWindowTitle("RichNVR - GStreamer Qt Player");
  resize(ClientSettings::VIDEOPLAYER_START_WIDTH,
         ClientSettings::VIDEOPLAYER_START_HEIGHT);

  // Quit application on ctrl + q
  auto *quitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), this);
  connect(quitShortcut, &QShortcut::activated, this, [this] {

    QCoreApplication::quit(); // finally close GUI
  });

  // Create video widget
  videoWidget = new QWidget(this);
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
  videoWidget->setAttribute(Qt::WA_NativeWindow); // ensure real native window
  (void)videoWidget->winId();                     // materialize window handle now
#endif
  setCentralWidget(videoWidget);
}

void VideoPlayer::playUri(const QString &source) {
  // Build a valid URI
  QString uri = source;
  if (!uri.contains("://"))
    uri = QUrl::fromLocalFile(uri).toString();

  lastUri_ = uri;
  playRetries_ = 0;

  // Teardown any previous pipeline
  if (pipeline) {
    if (busTimer_)
      busTimer_->stop();
    if (bus_) {
      gst_object_unref(bus_);
      bus_ = nullptr;
    } // <-- add this
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    pipeline = nullptr;
  }

  // Create playbin
  pipeline = gst_parse_launch(QString("playbin uri=%1").arg(uri).toStdString().c_str(), nullptr);
  
  // Disable CUDA decoders to avoid memory issues in WSL
  if (core::PathUtils::isWSLEnvironment() && pipeline) {
    GstElementFactory *nvh264dec = gst_element_factory_find("nvh264dec");
    GstElementFactory *nvh265dec = gst_element_factory_find("nvh265dec");
    
    if (nvh264dec) {
      gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(nvh264dec), GST_RANK_NONE);
      gst_object_unref(nvh264dec);
      std::cout << "[VideoPlayer] WSL: Disabled nvh264dec\n";
    }
    if (nvh265dec) {
      gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(nvh265dec), GST_RANK_NONE);
      gst_object_unref(nvh265dec);
      std::cout << "[VideoPlayer] WSL: Disabled nvh265dec\n";
    }
  }

  // Force TCP + bump jitterbuffer latency
  g_signal_connect(
      pipeline, "source-setup",
      G_CALLBACK(+[](GstElement * /*playbin*/, GstElement *src, gpointer) {
        if (g_str_has_prefix(G_OBJECT_TYPE_NAME(src), "GstRTSPSrc")) {
          // Try UDP on LAN (smoother). If you must keep TCP, leave the TCP
          // line. g_object_set(src, "protocols",
          // (gint)GST_RTSP_LOWER_TRANS_UDP, nullptr);
          g_object_set(src, "protocols", (gint)GST_RTSP_LOWER_TRANS_TCP,
                       nullptr);

          // Increase jitterbuffer size; 500–800ms is a good smoothing range
          g_object_set(src, "latency", 1200, nullptr); // was 200
          g_object_set(src, "drop-on-latency", TRUE,
                       nullptr); // drop late frames (avoid stalls)

          // Optional: more forgiving reconnects
          g_object_set(src, "timeout", (guint64)5 * GST_SECOND, nullptr);
        }
      }),
      nullptr);

  // After you create 'pipeline'
  bus_ = gst_element_get_bus(pipeline);

  // Sync handler stays (for window-handle). Keep yours, just ensure it’s active
  // on Windows.
  gst_bus_set_sync_handler(
      bus_,
      +[](GstBus *, GstMessage *msg, gpointer user_data) -> GstBusSyncReply {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT &&
            gst_is_video_overlay_prepare_window_handle_message(msg)) {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
          auto *self = static_cast<VideoPlayer *>(user_data);
          
          // WSL: Try window handle setting with error recovery
          if (core::PathUtils::isWSLEnvironment()) {
            std::cout << "[VideoSink] WSL: Attempting window handle setting with error recovery\n";
            // Continue to try setting window handle, but don't fail if it doesn't work
          }
          
          if (self->videoWidget) {
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
            // Ensure native window for both Windows and Linux
            self->videoWidget->setAttribute(Qt::WA_NativeWindow);
            (void)self->videoWidget->winId();
#endif
            auto *overlay = GST_VIDEO_OVERLAY(GST_MESSAGE_SRC(msg));
            
            // WSL: Set up comprehensive X11 error handler
            if (core::PathUtils::isWSLEnvironment()) {
              // Set global X11 error handler for entire GStreamer pipeline
              static auto wslErrorHandler = [](Display*, XErrorEvent* error) -> int {
                std::cout << "[VideoSink] WSL: Ignoring X11 error " << (int)error->error_code 
                         << " (request " << (int)error->request_code << ")\n";
                return 0; // Ignore all X11 errors in WSL
              };
              auto oldHandler = XSetErrorHandler(wslErrorHandler);
              
              gst_video_overlay_set_window_handle(
                  overlay, (guintptr)self->videoWidget->winId());
              gst_video_overlay_expose(overlay);
              
              // Keep the WSL error handler active for the entire pipeline lifetime
              // Don't restore old handler - let WSL error handler stay active
              std::cout << "[VideoSink] WSL: X11 error handler installed permanently\n";
            } else {
              gst_video_overlay_set_window_handle(
                  overlay, (guintptr)self->videoWidget->winId());
              gst_video_overlay_expose(overlay); // nudge first paint
            }
            return GST_BUS_DROP;
          }
#endif
        }
        return GST_BUS_PASS;
      },
      this, nullptr);

  // NEW: Qt timer that polls the bus in the GUI thread
  if (!busTimer_) {
    busTimer_ = new QTimer(this);
    busTimer_->setInterval(50);
    connect(busTimer_, &QTimer::timeout, this, [this] {
      if (!bus_)
        return;
      GstMessage *msg = nullptr;
      while ((msg = gst_bus_pop(bus_)) != nullptr) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
          GError* err = nullptr; gchar* dbg = nullptr;
          gst_message_parse_error(msg, &err, &dbg);
          const QString emsg =
              err ? QString::fromUtf8(err->message) : QString();
          qWarning() << "[GStreamer][ERROR]" << emsg
                     << "\nDebug:" << (dbg ? dbg : "");

          // quick, bounded retry if RTSP not yet ready
          if (lastUri_.startsWith("rtsp://", Qt::CaseInsensitive) &&
              playRetries_ < 3 &&
              (emsg.contains("404") ||
               emsg.contains("Not Found", Qt::CaseInsensitive) ||
               emsg.contains("Could not read from resource", Qt::CaseInsensitive) ||
               emsg.contains("Connection refused", Qt::CaseInsensitive) ||
               emsg.contains("Timed out", Qt::CaseInsensitive))) {
            const int delayMs = 300 + playRetries_ * 300; // 300,600,900
            playRetries_++;
            QTimer::singleShot(delayMs, this, [this] { playUri(lastUri_); });
          }
          if (err)
            g_error_free(err);
          if (dbg)
            g_free(dbg);
          break;
        }
        case GST_MESSAGE_WARNING: {
          GError* e = nullptr; gchar* dbg = nullptr;
          gst_message_parse_warning(msg, &e, &dbg);
          qWarning() << "[GST][WARN]" << (e ? e->message : "?")
                     << "\nDebug:" << (dbg ? dbg : "");
          if (e)
            g_error_free(e);
          if (dbg)
            g_free(dbg);
          break;
        }
        default:
          break;
        }
        gst_message_unref(msg);
      }
    });
  }
  busTimer_->start();

  // gst_object_unref(bus);

  // Before set_state(PLAYING)
  GstElement *videoSink = nullptr;

#ifdef Q_OS_WIN
  const char *sinks[] = {"d3d11videosink", "glimagesink", "autovideosink",
                         nullptr};
#elif defined(Q_OS_LINUX)
  const char **sinks = nullptr;
  
  if (core::PathUtils::isWSLEnvironment()) {
    // WSL environment - use appsink to avoid X11 issues entirely
    std::cout << "[VideoSink] WSL detected - using appsink with VideoSurface\n";
    
    videoSink = gst_element_factory_make("appsink", nullptr);
    if (videoSink) {
      appSink_ = videoSink; // Store reference
      
      g_object_set(videoSink,
                  "emit-signals", TRUE,
                  "sync", FALSE,
                  "max-buffers", 1,
                  "drop", TRUE,
                  nullptr);
      
      // Set caps for RGB format that Qt can handle
      GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                         "format", G_TYPE_STRING, "RGB",
                                         nullptr);
      g_object_set(videoSink, "caps", caps, nullptr);
      gst_caps_unref(caps);
      
      // Connect new-sample callback
      g_signal_connect(videoSink, "new-sample", 
                      G_CALLBACK(+[](GstElement*, gpointer user_data) -> GstFlowReturn {
        auto* self = static_cast<VideoPlayer*>(user_data);
        if (self->appSink_) {
          GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(self->appSink_));
          if (sample) {
            self->handleVideoFrame(sample);
            gst_sample_unref(sample);
          }
        }
        return GST_FLOW_OK;
      }), this);
      
      // Replace videoWidget with VideoSurface for WSL
      delete videoWidget;
      surface_ = new VideoSurface(this);
      videoWidget = surface_;
      setCentralWidget(videoWidget);
      
      std::cout << "[VideoSink] WSL: Created appsink with VideoSurface\n";
    }
  } else {
    // Regular Linux - use default sinks
    static const char *linuxSinks[] = {"glimagesink", "ximagesink", "autovideosink", nullptr};
    sinks = linuxSinks;
  }
#else
  const char *sinks[] = {"autovideosink", nullptr};
#endif

  for (const char **p = sinks; p && *p && !videoSink; ++p) {
    videoSink = gst_element_factory_make(*p, nullptr);
    if (videoSink) {
      std::cout << "[VideoSink] Using " << *p << "\n";
      
      // WSL-specific: set properties to make video sink more robust
      if (core::PathUtils::isWSLEnvironment()) {
        if (g_str_has_prefix(*p, "wayland")) {
          g_object_set(videoSink, "sync", FALSE, nullptr);
          std::cout << "[VideoSink] WSL: Set waylandsink sync=false\n";
        } else if (g_str_has_prefix(*p, "gl")) {
          g_object_set(videoSink, "sync", FALSE, "force-aspect-ratio", TRUE, nullptr);
          std::cout << "[VideoSink] WSL: Set glimagesink sync=false, force-aspect-ratio=true\n";
        } else if (g_str_has_prefix(*p, "xv") || g_str_has_prefix(*p, "xi")) {
          g_object_set(videoSink, 
                      "sync", FALSE, 
                      "force-aspect-ratio", TRUE,
                      "handle-events", FALSE,  // Don't handle X11 events
                      nullptr);
          std::cout << "[VideoSink] WSL: Set X11 sink sync=false, force-aspect-ratio=true, handle-events=false\n";
        }
      }
    } else {
      std::cout << "[VideoSink] Could not create " << *p
                << ", trying next...\n";
    }
  }
  if (videoSink) {
#ifdef Q_OS_WIN
    if (g_str_has_prefix(G_OBJECT_TYPE_NAME(videoSink), "GstD3D11")) {
      g_object_set(videoSink, "force-aspect-ratio", TRUE, "sync",
                   FALSE, // present as fast as possible
                   nullptr);
    }
#endif

    // Now that CUDA decoders are disabled in WSL, use video sink directly
    g_object_set(pipeline, "video-sink", videoSink, nullptr);
  } else {
    qWarning() << "No usable video sink found.";
    return;
  }

  /* ----------- start playback ------------ */
  // WSL-specific: ensure widget is fully realized and delay pipeline start
  if (core::PathUtils::isWSLEnvironment()) {
    if (videoWidget) {
      // Force window to be visible and processed
      show();
      videoWidget->show();
      videoWidget->setAttribute(Qt::WA_NativeWindow, true);
      videoWidget->winId(); // Force window handle creation
      
      // Process all pending events to ensure X11 window is fully created
      for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
        QThread::msleep(10); // Small delay for X11 to catch up
      }
      std::cout << "[VideoPlayer] WSL: Widget fully realized, winId=" 
                << (unsigned long)videoWidget->winId() << "\n";
    }
  }
  
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void VideoPlayer::toggleFullscreen() {
  if (isFullScreen()) {
    showNormal();
  } else {
    showFullScreen();
  }
}

void VideoPlayer::mouseDoubleClickEvent(QMouseEvent *event) {
  toggleFullscreen();
  QMainWindow::mouseDoubleClickEvent(
      event); // Optional: forward to base handler
}

void VideoPlayer::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case ClientSettings::VIDEOPLAYER_FULLSCREEN_KEY:
    toggleFullscreen();
    break;

  case ClientSettings::VIDEOPLAYER_EXIT_FULLSCREEN_KEY:
    if (isFullScreen())
      toggleFullscreen();
    break;

  case Qt::Key_Space:
    togglePause();
    break;

  case Qt::Key_Right:
    seekRelative(+ClientSettings::SEEK_SKIP_SECONDS); // skip forward 3s
    break;

  case Qt::Key_Left:
    seekRelative(-ClientSettings::SEEK_SKIP_SECONDS); // skip backward 3s
    break;

  default:
    QMainWindow::keyPressEvent(event);
    break;
  }
}

void VideoPlayer::togglePause() {
  if (!pipeline)
    return;

  GstState state;
  gst_element_get_state(pipeline, &state, nullptr, 0);

  if (state == GST_STATE_PLAYING) {
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
  } else if (state == GST_STATE_PAUSED) {
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
  }
}

void VideoPlayer::seekRelative(gint seconds) {
  if (!pipeline)
    return;

  gint64 position = 0;
  if (!gst_element_query_position(pipeline, GST_FORMAT_TIME, &position))
    return;

  position += seconds * GST_SECOND;
  if (position < 0)
    position = 0;

  gst_element_seek_simple(
      pipeline, GST_FORMAT_TIME,
      static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
      position);
}

void VideoPlayer::handleVideoFrame(GstSample* sample) {
  GstBuffer* buffer = gst_sample_get_buffer(sample);
  GstCaps* caps = gst_sample_get_caps(sample);
  
  if (!buffer || !caps) return;
  
  GstStructure* structure = gst_caps_get_structure(caps, 0);
  gint width, height;
  
  if (!gst_structure_get_int(structure, "width", &width) ||
      !gst_structure_get_int(structure, "height", &height)) {
    return;
  }
  
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    // Create QImage from RGB data
    QImage image(map.data, width, height, QImage::Format_RGB888);
    
    // Send to VideoSurface on GUI thread
    if (surface_) {
      QMetaObject::invokeMethod(surface_, "setFrame", Qt::QueuedConnection, Q_ARG(QImage, image));
    }
    
    gst_buffer_unmap(buffer, &map);
  }
}

VideoPlayer::~VideoPlayer() {
  if (busTimer_) {
    if (busTimer_->thread() == QThread::currentThread()) {
      busTimer_->stop();
    } else {
      // Timer is on different thread, stop it safely and wait
      busTimer_->moveToThread(QThread::currentThread());
      busTimer_->stop();
    }
    busTimer_->deleteLater();
    busTimer_ = nullptr;
  }
  if (pipeline) {
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }
  if (bus_) {
    gst_object_unref(bus_);
    bus_ = nullptr;
  }
  if (pipeline) {
    gst_object_unref(pipeline);
    pipeline = nullptr;
  }
}
