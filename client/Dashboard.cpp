#include "Dashboard.h"
#include "CamerasInfoDialog.h"
#include "ClientSettings.h"
#include "FloatingOverlay.h"
#include "OverlayBox.h"
#include "PathUtils.h"
#include "Remote.h"
#include "RemoteServerDialog.h"
#include "Settings.h"
#include "VideoPlayer.h"
#include "VideoWrapper.h"
#include "conversations.h"
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkReply>
#include <QPalette>
#include <QPixmap>
#include <QSaveFile>
#include <QScreen>
#include <QSet>
#include <QShortcut>
#include <QSize>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <iostream>
#include <nlohmann/json.hpp>


static QString camerasJsonPath() {
  const QString base = QCoreApplication::applicationDirPath() + "/config";
  QDir().mkpath(base);
  return base + "/cameras.json";
}

Dashboard::Dashboard(QWidget *parent)
    : QMainWindow(parent), settings_("settings.json") {
  setWindowTitle("RichNVR - GStreamer videostream Dashboard");
  resize(ClientSettings::DASHBOARD_START_WIDTH,
         ClientSettings::DASHBOARD_START_HEIGHT);

  // Quit application on ctrl + q
  auto *quitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), this);
  connect(quitShortcut, &QShortcut::activated, qApp, [] {

    QCoreApplication::quit(); // finally close GUI
  });

  // Central widget and layout
  auto *central = new QWidget(this);
  auto *layout = new QVBoxLayout(central);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // Create and center the logo
  logo = new QLabel(this);
  QPixmap pix("../assets/logo.png"); // Adjust if needed
  logo->setPixmap(
      pix.scaled(250, 250, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  logo->setAlignment(Qt::AlignCenter);
  logo->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

  // Add logo to layout
  layout->addWidget(logo, 0, Qt::AlignCenter);

  // Add color that matches logo
  QPalette pal = central->palette();
  pal.setColor(QPalette::Window, QColor("#e9e9e9")); // Light gray
  central->setAutoFillBackground(true);
  central->setPalette(pal);

  // Add widgets dynamically later, or append your stream grid here
  central->setLayout(layout);
  setCentralWidget(central);

  createMenuBar();
  restoreWindowFromSettings();
  loadInitialCameras();
}

void Dashboard::contextMenuEvent(QContextMenuEvent *event) {
  QMenu menu(this);

  QAction *addStreamAction = menu.addAction("Add Stream");
  addStreamAction->setToolTip("Add a new camera stream to the dashboard");

  // Show/Hide Menu Bar toggle
  QAction *toggleMenuAction =
      menu.addAction(menuVisible ? "Hide Menu Bar" : "Show Menu Bar");
  toggleMenuAction->setToolTip("Show or hide the top menu bar");

  QAction *quitAction = menu.addAction("Quit (Ctrl+Q)");
  quitAction->setToolTip("Close all windows and exit the application");

  QAction *selectedAction = menu.exec(event->globalPos());

  if (selectedAction == addStreamAction) {
    onAddStreamTriggered();
    std::cout << "[Dashboard] Add Stream triggered";
  } else if (selectedAction == toggleMenuAction) {
    menuVisible = !menuVisible;
    if (menuBar) {
      menuBar->setVisible(menuVisible);
    }

    toggleMenuAction->setText(menuVisible ? "Hide Menu Bar" : "Show Menu Bar");
  } else if (selectedAction == quitAction) {
    QNetworkAccessManager nm;
    QNetworkRequest req(QUrl(QStringLiteral("http://%1:8080/shutdown")
                                 .arg(Remote::getCurrentHost())));
    QNetworkReply *rep = nm.post(req, QByteArray(""));

    QEventLoop loop;
    connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    std::cout
        << "[GUI] Quit requested, server reply:"
        << rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        << rep->readAll();

    rep->deleteLater();

    qApp->quit();
  }
}

static QUrl makeBaseUrl(const QString &server) {
  QString s = server.trimmed();
  if (s.isEmpty())
    s = "127.0.0.1:8080"; // fallback default
  if (!s.startsWith("http://") && !s.startsWith("https://"))
    s.prepend("http://");
  QUrl u(s);
  if (u.port() == -1)
    u.setPort(8080);
  return u;
}

void Dashboard::onAddStreamTriggered() {
  auto *dialog = new AddStreamDialog(nullptr);
  dialog->setAttribute(Qt::WA_DeleteOnClose, true);
  dialog->setWindowModality(Qt::NonModal);
  dialog->setModal(false);
  dialog->setWindowFlag(Qt::Window, true); // make it a top-level window
  dialog->show();                          // modeless

  connect(dialog, &QDialog::accepted, this, [this, dialog]() {
    const QString server = dialog->targetServer();
    const QString camID = dialog->cameraId();
    const QString url = dialog->streamUrl();

    const bool useSize = dialog->useMotionFrameSize();
    const QSize mfSize = dialog->motionFrameSize();   // may be 0x0
    const float mfScale = dialog->motionFrameScale(); // used if !useSize
    const float dash = dialog->dashboardDisplayScale();

    // 1) Tell the server
    const bool ok = sendAddCameraToServer(
        server, camID, url, dialog->segment(), dialog->motionFrame(),
        dialog->liveProxied(),
        useSize ? mfSize : QSize(), // send size only if chosen
        useSize ? 0.0f : mfScale,   // send scale only if chosen
        dialog->noiseThreshold(), dialog->motionThreshold(),
        dialog->motionMinHits(), dialog->motionDecay(),
        dialog->motionArrowScale(), dialog->motionArrowThickness());

    if (!ok) {
      QMessageBox::warning(
          this, "Could not add camera",
          "Unable to contact RichServer. Please check your server settings.");
      return;
    }

    // 2) Choose display size for the UI (apply dashboard size factor)
    int baseW = 0, baseH = 0;
    if (useSize && mfSize.width() > 0 && mfSize.height() > 0) {
      baseW = mfSize.width();
      baseH = mfSize.height();
    } else {
      const QSize discovered = mfSize; // dialog fills these if user probed
      if (discovered.width() > 0 && discovered.height() > 0) {
        baseW = static_cast<int>(discovered.width() * mfScale);
        baseH = static_cast<int>(discovered.height() * mfScale);
      } else {
        // fallback guess (if no probe)
        baseW = static_cast<int>(1280 * mfScale);
        baseH = static_cast<int>(720 * mfScale);
      }
    }

    const int drawW = std::max(1, static_cast<int>(baseW * dash));
    const int drawH = std::max(1, static_cast<int>(baseH * dash));

    // If proxied, switch the URL we *display* to the RTSP relay
    QString effectiveUrl = url;
    if (dialog->liveProxied()) {
      const QUrl srv = makeBaseUrl(server); // http(s)://host:8080 → parse host
      const QString host = srv.host();

      // sanitize cam id for path segment
      const QString sanitized = QString::fromStdString(
          core::PathUtils::sanitizeCameraName(camID.toStdString()));

      QUrl rtsp;
      rtsp.setScheme("rtsp");
      rtsp.setHost(host);
      rtsp.setPort(settings_.live_rtsp_proxy_port());
      rtsp.setPath("/cam/" + sanitized);
      effectiveUrl = rtsp.toString();

      std::cout << "[Dashboard] Using proxied RTSP URL: " << effectiveUrl
                << '\n';

      waitForRtspReady(QUrl(effectiveUrl),
                       5000); // non-blocking UI? see note below
    }

    addCamera(effectiveUrl, camID, drawW, drawH);

    std::cout << "[Dashboard] Added camera id:" << camID << " server:" << server
              << " url-sent:" << url << " url-used:" << effectiveUrl
              << " proxied:" << (dialog->liveProxied() ? "1" : "0")
              << " draw:" << drawW << "x" << drawH << '\n';
  });
}

bool Dashboard::waitForRtspReady(const QUrl &rtspUrl, int overallTimeoutMs) {
  const QString host = rtspUrl.host();
  const int port = rtspUrl.port(554);
  const QString full = rtspUrl.toString();

  QElapsedTimer timer;
  timer.start();

  while (timer.elapsed() < overallTimeoutMs) {
    QTcpSocket sock;
    sock.connectToHost(host, port);
    if (!sock.waitForConnected(1000)) {
      QThread::msleep(150);
      continue;
    }

    // Ask for SDP at the exact path; 200 means mount is ready.
    const QByteArray req = QString("DESCRIBE %1 RTSP/1.0\r\n"
                                   "CSeq: 1\r\n"
                                   "Accept: application/sdp\r\n"
                                   "User-Agent: RichNVR/1.0\r\n\r\n")
                               .arg(full)
                               .toUtf8();

    sock.write(req);
    sock.flush();

    if (sock.waitForReadyRead(1000)) {
      const QByteArray resp = sock.readAll();
      if (resp.startsWith("RTSP/1.0 200")) {
        std::cout << "[RTSP] Ready: " << full.toStdString() << "\n";
        return true;
      }
      // 404/454 etc → not ready yet
    }
    QThread::msleep(200);
  }

  std::cout << "[RTSP] Not ready within " << overallTimeoutMs
            << " ms: " << rtspUrl.toString().toStdString() << "\n";
  return false;
}

void Dashboard::addCamera(const QString &url, const QString &camID, int w,
                          int h) {
  if (logo) {
    logo->hide(); // 👈 Hide logo once the first stream is added
  }

  /* ---------- Overlay (Linux only) ---------- */
#if defined(Q_OS_LINUX)

  // Create player and set size
  auto *player = new VideoPlayer(this);
  player->setFixedSize(w, h);
  // Note: playUri() will be called after widget setup is complete

  // Create wrapper widget to host video and layout
  QWidget *wrapper = nullptr;
  
  if (core::PathUtils::isWSLEnvironment()) {
    // WSL needs VideoWrapper for hover events with FloatingOverlay
    wrapper = new VideoWrapper(this);
    wrapper->setFixedSize(w, h);
    static_cast<VideoWrapper*>(wrapper)->setVideoWidget(player);
  } else {
    // Regular Linux uses simple QWidget
    wrapper = new QWidget(this);
    wrapper->setFixedSize(w, h);

    auto *wrapperLayout = new QVBoxLayout(wrapper);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(player);
  }

  wrapper->show();
  
  // Use different overlay types based on environment
  if (core::PathUtils::isWSLEnvironment()) {
    // WSL uses context menu instead of FloatingOverlay to avoid positioning issues
    std::cout << "[Dashboard] WSL detected - using context menu for controls\n";
    
    auto *videoWrapper = static_cast<VideoWrapper*>(wrapper);
    
    // Set up context menu with FloatingOverlay functionality
    videoWrapper->setCameraInfo(camID, url); // Pass camera info for context menu
    
    // Connect camera deletion signal
    connect(videoWrapper, &VideoWrapper::cameraDeleted, this, &Dashboard::removeCameraFromUI);
    
    camWidgets_.insert(camID, CameraEntry{camID, url, w, h, /*x*/ -1, /*y*/ -1,
                                          wrapper, nullptr}); // No OverlayBox for WSL
  } else {
    // Regular Linux - use OverlayBox
    auto *overlay = new OverlayBox(camID, url, this);
    overlay->setFixedSize(w, h);
    overlay->move(wrapper->mapToGlobal(QPoint(0, 0)));
    overlay->raise();
    overlay->show();

    camWidgets_.insert(camID, CameraEntry{camID, url, w, h, /*x*/ -1, /*y*/ -1,
                                          wrapper, overlay});

    connect(overlay, &OverlayBox::droppedAt, this, [=](const QPoint &global) {
      const QPoint local = this->mapFromGlobal(global);
      wrapper->move(local);
      overlay->move(global);
      wrapper->raise();
      overlay->raise();

      saveSingleCameraGeometry(camID, wrapper->pos(), wrapper->size());
    });

    // Connect camera deletion signal
    connect(overlay, &OverlayBox::cameraDeleted, this, &Dashboard::removeCameraFromUI);
  }

  // IMPORTANT: start playback after widget hierarchy is set up
  // Defer playback to ensure widgets are properly embedded and visible
  QTimer::singleShot(0, player, [player, url] { 
    std::cout << "[Dashboard] Starting deferred playback for: " << url.toStdString() << std::endl;
    player->playUri(url); 
  });
  
#else // MacOS/Windows
  // Create wrapper first
  auto *wrapper = new VideoWrapper(this);
  wrapper->setFixedSize(w, h);
  wrapper->show();

  // Create the player as a child of the wrapper (avoid re-parent later)
  auto *player = new VideoPlayer(wrapper); // child from the start
  player->setFixedSize(w, h);
  player->setWindowFlags(Qt::Widget);
  wrapper->setVideoWidget(player);

  // Create floating overlay window
  auto *overlay = new FloatingOverlay(camID, url, wrapper);
  overlay->adjustSize();
  overlay->hide(); // Initially hidden

  // Track and update overlay position
  auto updateOverlayPosition = [=](const QPoint &globalPos) {
    overlay->move(globalPos.x() + (wrapper->width() - overlay->width()) / 2,
                  globalPos.y() + (wrapper->height() - overlay->height()) - 10);
  };

  // Initial placement
  updateOverlayPosition(wrapper->mapToGlobal(QPoint(0, 0)));

  // Keep it updated while dragging
  connect(wrapper, &VideoWrapper::positionChanged, this, updateOverlayPosition);

  // Hover logic with delayed hide
  auto *hoverTimer = new QTimer(this);
  hoverTimer->setInterval(500);
  hoverTimer->setSingleShot(true);

  auto showOverlay = [=] {
    overlay->show();
    hoverTimer->stop();
  };
  auto tryHideOverlay = [=] { hoverTimer->start(); };

  connect(hoverTimer, &QTimer::timeout, this, [=]() {
    const QPoint global = QCursor::pos();
    if (!wrapper->rect().contains(wrapper->mapFromGlobal(global)) &&
        !overlay->rect().contains(overlay->mapFromGlobal(global))) {
      overlay->hide();
    }
  });
  connect(wrapper, &VideoWrapper::entered, this, showOverlay);
  connect(wrapper, &VideoWrapper::left, this, tryHideOverlay);
  overlay->installEventFilter(this);

  // Register for geometry save/restore
  camWidgets_.insert(camID, CameraEntry{camID, url, w, h, /*x*/ -1, /*y*/ -1,
                                        wrapper, overlay});
  repositionOverlay(wrapper, overlay);
  wrapper->raise();
  overlay->raise();

  // IMPORTANT: start playback after the native window exists
  // Defer one tick so QWidget creates the HWND before GStreamer asks for it.
  QTimer::singleShot(0, player, [player, url] { player->playUri(url); });
#endif

  std::cout << "[Dashboard] Added stream with size:" << w << "x" << h;
}

void Dashboard::repositionOverlay(QWidget *wrapper, QWidget *overlay) {
  if (!wrapper || !overlay)
    return;

  // Check if this is a FloatingOverlay (used in WSL and Windows)
  if (qobject_cast<FloatingOverlay*>(overlay)) {
    // For FloatingOverlay, use top-center positioning like WSL logic
    const QPoint g = wrapper->mapToGlobal(QPoint(0, 0));
    int overlayX = g.x() + (wrapper->width() - overlay->width()) / 2;
    int overlayY = g.y() + 10; // 10px from top
    overlay->move(overlayX, overlayY);
    overlay->raise();
    std::cout << "[Dashboard] repositionOverlay (FloatingOverlay): " << overlayX << "," << overlayY << std::endl;
  } else {
    // Regular OverlayBox positioning for standard Linux
    overlay->move(wrapper->mapToGlobal(QPoint(0, 0)));
    overlay->raise();
  }
}

void Dashboard::removeCameraFromUI(const QString &cameraID) {
  std::cout << "[Dashboard] Removing camera from UI: " << cameraID.toStdString() << std::endl;
  
  auto it = camWidgets_.find(cameraID);
  if (it == camWidgets_.end()) {
    std::cout << "[Dashboard] Camera not found in widgets: " << cameraID.toStdString() << std::endl;
    return;
  }
  
  CameraEntry entry = it.value();
  
  // Clean up wrapper widget
  if (entry.wrapper) {
    entry.wrapper->hide();
    entry.wrapper->deleteLater();
    std::cout << "[Dashboard] Cleaned up wrapper for camera: " << cameraID.toStdString() << std::endl;
  }
  
  // Clean up overlay widget (if it exists for non-WSL systems)
  if (entry.overlay) {
    entry.overlay->hide();
    entry.overlay->deleteLater();
    std::cout << "[Dashboard] Cleaned up overlay for camera: " << cameraID.toStdString() << std::endl;
  }
  
  // Remove from our tracking
  camWidgets_.erase(it);
  knownCamIds_.remove(cameraID);
  
  // Update cameras.json config file to remove this camera
  updateCamerasJsonAfterDelete(cameraID);
  
  std::cout << "[Dashboard] Successfully removed camera: " << cameraID.toStdString() 
            << ". Remaining cameras: " << camWidgets_.size() << std::endl;
}

void Dashboard::updateCamerasJsonAfterDelete(const QString &cameraID) {
  const QString configPath = camerasJsonPath();
  std::cout << "[Dashboard] Updating cameras.json after deletion: " << cameraID.toStdString() << std::endl;
  
  // Read current cameras from config file
  QVector<CameraEntry> currentCameras = readLocalCamerasJson(configPath);
  std::cout << "[Dashboard] Read " << currentCameras.size() << " cameras from config file" << std::endl;
  
  // Filter out the deleted camera
  QVector<CameraEntry> updatedCameras;
  bool found = false;
  for (const auto &camera : currentCameras) {
    if (camera.id != cameraID) {
      updatedCameras.append(camera);
    } else {
      found = true;
      std::cout << "[Dashboard] Found camera to remove from config: " << cameraID.toStdString() << std::endl;
    }
  }
  
  if (!found) {
    std::cout << "[Dashboard] Camera " << cameraID.toStdString() 
              << " not found in config file, nothing to remove" << std::endl;
    return;
  }
  
  // Save the updated camera list back to the config file (replace entire list)
  replaceCamerasJson(configPath, updatedCameras);
  std::cout << "[Dashboard] Updated cameras.json: removed " << cameraID.toStdString() 
            << ". Cameras remaining in config: " << updatedCameras.size() << std::endl;
}

void Dashboard::createMenuBar() {
  if (menuBar)
    return;

  menuBar = new QMenuBar(this);
  setMenuBar(menuBar);
  // menuBar->setNativeMenuBar(false);

  // File Menu
  QMenu *fileMenu = menuBar->addMenu("File");
  QAction *setRemoteServer = fileMenu->addAction("Set remote server");
  QAction *exitAction = fileMenu->addAction("Quit (Ctrl+Q)");

  connect(exitAction, &QAction::triggered, this, [this]() {
    QNetworkAccessManager nm;
    QNetworkRequest req(QUrl(QStringLiteral("http://%1:8080/shutdown")
                                 .arg(Remote::getCurrentHost())));
    QNetworkReply *rep = nm.post(req, QByteArray(""));

    QEventLoop loop;
    connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    std::cout
        << "[GUI] Ctrl+Q / Quit chosen, server reply:"
        << rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()
        << rep->readAll();

    rep->deleteLater();
    QCoreApplication::quit();
  });

  connect(setRemoteServer, &QAction::triggered, this, [this] {
    RemoteServerDialog dlg(this);
    dlg.exec();
  });

  // View Menu
  QMenu *viewMenu = menuBar->addMenu("View");

  QAction *toggleMenuAction = viewMenu->addAction("Hide Menu Bar");
  connect(toggleMenuAction, &QAction::triggered, this,
          [this, toggleMenuAction] {
            menuVisible = !menuVisible;
            menuBar->setVisible(menuVisible);
            toggleMenuAction->setText(menuVisible ? "Hide Menu Bar"
                                                  : "Show Menu Bar");
          });

  QAction *camInfoAct = viewMenu->addAction("Cameras Info");
  connect(camInfoAct, &QAction::triggered, this, [this] {
    auto *dialog = new CamerasInfoDialog(nullptr);
    dialog->setAttribute(
        Qt::WA_DeleteOnClose); // optional: auto-delete on close
    dialog->show();
  });

  // Help Menu
  QMenu *helpMenu = menuBar->addMenu("Help");
  QAction *aboutAction = helpMenu->addAction("About RichNVR");
  connect(aboutAction, &QAction::triggered, this, [] {
    QMessageBox::about(nullptr, "About RichNVR",
                       "RichNVR - GStreamer Video Dashboard\n© 2025 Rich-bit");
  });
}

bool Dashboard::sendAddCameraToServer(
    const QString &server, const QString &id, const QString &url, bool segment,
    bool motionFrame, bool liveProxied, const QSize &motionFrameSize,
    float motionFrameScale, float noiseThreshold, float motionThreshold,
    int motionMinHits, int motionDecay, float motionArrowScale,
    float motionArrowThickness) {

  QUrl reqUrl = makeBaseUrl(server);
  reqUrl.setPath("/add_camera");

  std::cout << "[Dashboard] /add_camera request to: " << reqUrl.toString().toStdString() << std::endl;

  QNetworkRequest req(reqUrl);
  req.setHeader(QNetworkRequest::KnownHeaders::ContentTypeHeader,
                "application/x-www-form-urlencoded");

  QUrlQuery form;
  form.addQueryItem("name", id);
  form.addQueryItem("uri", url);

  if (segment)
    form.addQueryItem("segment", "1");
  if (motionFrame)
    form.addQueryItem("motion_frame", "1");
  if (liveProxied)
    form.addQueryItem("live555proxied", "1");

  if (motionFrameSize.width() > 0 && motionFrameSize.height() > 0) {
    form.addQueryItem("motion_frame_size", QString("%1x%2")
                                               .arg(motionFrameSize.width())
                                               .arg(motionFrameSize.height()));
  }

  auto num = [](double v) { return QString::number(v, 'g', 8); };
  form.addQueryItem("motion_frame_scale", num(motionFrameScale));
  form.addQueryItem("noise_threshold", num(noiseThreshold));
  form.addQueryItem("motion_threshold", num(motionThreshold));
  form.addQueryItem("motion_min_hits", QString::number(motionMinHits));
  form.addQueryItem("motion_decay", QString::number(motionDecay));
  form.addQueryItem("motion_arrow_scale", num(motionArrowScale));
  form.addQueryItem("motion_arrow_thickness", num(motionArrowThickness));

  QEventLoop loop;
  QNetworkReply *reply =
      manager_.post(req, form.query(QUrl::FullyEncoded).toUtf8());
  QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();

  const int http =
      reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  const bool ok = (reply->error() == QNetworkReply::NoError && http == 200);

  if (!ok) {
    std::cout << "[GUI] /add_camera failed: " << reply->errorString()
              << " (HTTP " << http << ")\n"
              << QString::fromUtf8(reply->readAll()) << '\n';
  } else {
    std::cout << "[GUI] /add_camera OK: " << QString::fromUtf8(reply->readAll())
              << '\n';
  }

  reply->deleteLater();
  return ok;
}

bool Dashboard::eventFilter(QObject *obj, QEvent *event) {
  if (auto *overlay = qobject_cast<FloatingOverlay *>(obj)) {
    switch (event->type()) {
    case QEvent::Enter:
      overlay->show(); // same as showOverlay
      return true;
    case QEvent::Leave:
      QTimer::singleShot(0, this, [=]() {
        // Give mouse position time to update
        const QPoint globalCursor = QCursor::pos();
        if (!overlay->rect().contains(overlay->mapFromGlobal(globalCursor)))
          overlay->hide();
      });
      return true;
    default:
      break;
    }
  }
  return QWidget::eventFilter(obj, event);
}
QString Dashboard::readServerFromSettingsJson(const QString &path) {
  std::cout << "[Dashboard] Reading server from: " << path << '\n';

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    std::cout << "[Dashboard] Could not open settings file.\n";
    return {};
  }
  const auto doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) {
    std::cout << "[Dashboard] settings.json is not a JSON object.\n";
    return {};
  }
  const auto obj = doc.object();
  const QStringList keys = {"server_address", "server", "remote_server"};
  for (const auto &k : keys) {
    if (obj.contains(k) && obj.value(k).isString()) {
      const auto s = obj.value(k).toString().trimmed();
      std::cout << "[Dashboard] Found server under key '" << k << "': " << s
                << '\n';
      return s;
    }
  }
  std::cout << "[Dashboard] No server key found in settings.json.\n";
  return {};
}

QVector<CameraEntry> Dashboard::fetchServerCameras(const QString &server) {
  QVector<CameraEntry> out;
  if (server.isEmpty()) {
    std::cout << "[HTTP] No server provided, skipping /get_cameras.\n";
    return out;
  }

  QUrl url = makeBaseUrl(server);
  url.setPath("/get_cameras");
  std::cout << "[HTTP] GET " << url.toString() << '\n';

  QNetworkRequest req(url);
  QEventLoop loop;
  QNetworkReply *rep = manager_.get(req);
  connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();

  const int http =
      rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (rep->error() != QNetworkReply::NoError || http != 200) {
    std::cout << "[HTTP] /get_cameras failed: " << rep->errorString()
              << " (HTTP " << http << ")\n";
    rep->deleteLater();
    return out;
  }

  const QByteArray payload = rep->readAll();
  rep->deleteLater();

  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &pe);
  if (pe.error != QJsonParseError::NoError) {
    std::cout << "[JSON] Parse error at " << pe.offset << ": "
              << pe.errorString() << '\n';
    return out;
  }
  if (!doc.isArray()) {
    std::cout << "[JSON] /get_cameras returned non-array payload.\n";
    return out;
  }

  const QJsonArray arr = doc.array();
  std::cout << "[HTTP] /get_cameras returned " << arr.size() << " camera(s).\n";

  for (const auto &v : arr) {
    if (!v.isObject())
      continue;
    const QJsonObject j = v.toObject();

    const QString id = j.value("name").toString();
    const QString uri = j.value("uri").toString();
    const QString proxPath = j.value("live_proxied_rtsp_path").isNull()
                                 ? QString()
                                 : j.value("live_proxied_rtsp_path").toString();

    int w = 0, h = 0;
    if (j.contains("motion_frame_size") &&
        j.value("motion_frame_size").isArray()) {
      const auto sz = j.value("motion_frame_size").toArray();
      if (sz.size() == 2) {
        w = sz.at(0).toInt(0);
        h = sz.at(1).toInt(0);
      }
    }
    const double scale = j.value("motion_frame_scale").toDouble(0.70);

    int drawW = 0, drawH = 0;
    if (w > 0 && h > 0) {
      drawW = w;
      drawH = h;
    } else {
      drawW = int(1280 * scale);
      drawH = int(720 * scale);
    }

    CameraEntry ce;
    ce.id = id;
    ce.url =
        proxPath.isEmpty()
            ? uri
            : QString("rtsp://%1:%2/%3")
                  .arg(makeBaseUrl(server).host())
                  .arg(settings_.live_rtsp_proxy_port())
                  .arg(proxPath.startsWith('/') ? proxPath.mid(1) : proxPath);
    ce.w = std::max(1, drawW);
    ce.h = std::max(1, drawH);

    if (!ce.id.isEmpty() && !ce.url.isEmpty()) {
      std::cout << "[Dashboard] Server camera: id=" << ce.id
                << " url=" << ce.url << " size=" << ce.w << "x" << ce.h << '\n';
      out.push_back(std::move(ce));
    }
  }

  return out;
}

QVector<CameraEntry> Dashboard::readLocalCamerasJson(const QString &path) {
  QVector<CameraEntry> out;

  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    std::cout << "[JSON] cameras.json open failed: " << path << '\n';
    return out;
  }

  const QByteArray data = f.readAll();
  f.close();

  QJsonParseError perr;
  QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
  if (perr.error != QJsonParseError::NoError) {
    std::cout << "[JSON] parse error in " << path << ": " << perr.errorString()
              << '\n';
    return out;
  }

  QJsonArray arr;
  if (doc.isArray()) {
    arr = doc.array();
    std::cout << "[JSON] cameras.json root is array. size=" << arr.size()
              << '\n';
  } else if (doc.isObject()) {
    auto obj = doc.object();
    if (obj.contains("cameras") && obj["cameras"].isArray()) {
      arr = obj["cameras"].toArray();
      std::cout << "[JSON] cameras.json root object with 'cameras' array. size="
                << arr.size() << '\n';
    } else {
      std::cout << "[JSON] cameras.json object has no 'cameras' array.\n";
      return out;
    }
  } else {
    std::cout << "[JSON] cameras.json not array/object.\n";
    return out;
  }

  for (const auto &v : arr) {
    if (!v.isObject())
      continue;
    const auto o = v.toObject();

    CameraEntry ce;
    ce.id = o.value("id").toString();
    ce.url = o.value("url").toString();
    ce.w = o.value("w").toInt(0);
    ce.h = o.value("h").toInt(0);
    ce.x = o.value("x").toInt(-1);
    ce.y = o.value("y").toInt(-1);

    if (ce.id.isEmpty()) {
      std::cout << "[JSON] skipping entry without id\n";
      continue;
    }
    out.push_back(ce);
  }

  std::cout << "[JSON] parsed " << out.size() << " camera entries from " << path
            << '\n';
  return out;
}

void Dashboard::saveCamerasJson(const QString &path,
                                const QVector<CameraEntry> &toMerge) {
  // Load existing
  QHash<QString, CameraEntry> map;
  for (const auto &ce : readLocalCamerasJson(path)) {
    map.insert(ce.id, ce);
  }
  // Merge/overwrite with new data
  for (const auto &ce : toMerge) {
    map[ce.id] = ce;
  }

  // Serialize as { "cameras": [ ... ] }
  QJsonArray arr;
  for (const auto &ce : map) {
    QJsonObject o;
    o["id"] = ce.id;
    o["url"] = ce.url;
    o["w"] = ce.w;
    o["h"] = ce.h;
    o["x"] = ce.x;
    o["y"] = ce.y;
    arr.push_back(o);
  }
  QJsonObject root;
  root["cameras"] = arr;

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cout << "[JSON] save failed (open): " << path << '\n';
    return;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.close();
  std::cout << "[Dashboard] Saved " << arr.size() << " camera(s) to " << path
            << '\n';
}

void Dashboard::replaceCamerasJson(const QString &path, const QVector<CameraEntry> &cameras) {
  // Replace entire camera list (used for deletion)
  std::cout << "[JSON] Replacing cameras.json with " << cameras.size() << " cameras" << std::endl;
  
  QJsonArray arr;
  for (const auto &ce : cameras) {
    QJsonObject o;
    o["id"] = ce.id;
    o["url"] = ce.url;
    o["w"] = ce.w;
    o["h"] = ce.h;
    o["x"] = ce.x;
    o["y"] = ce.y;
    arr.push_back(o);
  }
  QJsonObject root;
  root["cameras"] = arr;

  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cout << "[JSON] replace failed (open): " << path.toStdString() << std::endl;
    return;
  }
  
  const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (f.write(data) != data.size()) {
    std::cout << "[JSON] replace failed (write): " << path.toStdString() << std::endl;
    return;
  }
  f.close();
  std::cout << "[JSON] Successfully replaced cameras.json with " << cameras.size() << " camera(s)" << std::endl;
}

void Dashboard::loadInitialCameras() {
  std::cout << "[Dashboard] ==== Initial camera load start ====\n";

  QVector<CameraEntry> added;

  // Build a geometry map from local file first
  const auto localAll = readLocalCamerasJson("config/cameras.json");
  QHash<QString, QPoint> localPos;
  for (const auto &ce : localAll) {
    if (ce.x >= 0 && ce.y >= 0)
      localPos.insert(ce.id, QPoint(ce.x, ce.y));
  }

  std::cout << "[Dashboard] Local positions available for " << localPos.size()
            << " camera(s)\n";

  // 1) Server cameras
  const QString server = readServerFromSettingsJson("config/settings.json");
  if (!server.isEmpty()) {
    const auto srvCams = fetchServerCameras(server);
    std::cout << "[Dashboard] Considering " << srvCams.size()
              << " server camera(s).\n";
    for (auto ce : srvCams) {
      if (knownCamIds_.contains(ce.id)) {
        std::cout << "[Dashboard] Skipping duplicate (server): " << ce.id
                  << '\n';
        continue;
      }

      addCamera(ce.url, ce.id, ce.w, ce.h);
      knownCamIds_.insert(ce.id);

      // If we have saved geometry locally, apply it
      if (localPos.contains(ce.id)) {
        const QPoint p = localPos.value(ce.id);
        std::cout << "[Dashboard] Found saved coords for " << ce.id << ": ("
                  << p.x() << "," << p.y() << ")\n";

        if (auto cw = camWidgets_.value(ce.id); cw.wrapper) {
          QWidget *w = cw.wrapper;
          QWidget *o = cw.overlay; // May be nullptr for WSL

          ce.x = p.x();
          ce.y = p.y();

          std::cout << "[Dashboard] Restoring " << ce.id
                    << " to local=" << QString("(%1,%2)").arg(ce.x).arg(ce.y)
                    << '\n';

          w->move(p);

          std::cout << "[Dashboard] After move wrapper local="
                    << QString("(%1,%2)").arg(w->pos().x()).arg(w->pos().y())
                    << " global="
                    << QString("(%1,%2)")
                           .arg(w->mapToGlobal(QPoint(0, 0)).x())
                           .arg(w->mapToGlobal(QPoint(0, 0)).y())
                    << '\n';

          // Reposition overlay immediately (only if overlay exists)
          if (o) {
            repositionOverlay(w, o);
          } else {
            std::cout << "[Dashboard] Skipping overlay repositioning (WSL - no overlay)" << std::endl;
          }

          // Optionally defer once to let layouts settle
          QTimer::singleShot(0, this, [this, id = ce.id]() {
            const auto cw2 = camWidgets_.value(id);
            if (!cw2.wrapper) {
              std::cout << "[Dashboard] Deferred: missing wrapper for " << id << '\n';
              return;
            }
            std::cout << "[Dashboard] Deferred check for " << id
                      << " wrapper local="
                      << QString("(%1,%2)")
                             .arg(cw2.wrapper->pos().x())
                             .arg(cw2.wrapper->pos().y())
                      << " global="
                      << QString("(%1,%2)")
                             .arg(cw2.wrapper->mapToGlobal(QPoint(0, 0)).x())
                             .arg(cw2.wrapper->mapToGlobal(QPoint(0, 0)).y())
                      << '\n';
            // Only reposition overlay if it exists (not null for WSL)
            if (cw2.overlay) {
              repositionOverlay(cw2.wrapper, cw2.overlay);
            }
          });

        } else {
          std::cout << "[Dashboard] WARN: wrapper/overlay missing for " << ce.id
                    << '\n';
        }
      }

      added.push_back(ce);
    }
  } else {
    std::cout << "[Dashboard] No server configured in settings.json.\n";
  }

  // 2) Local cameras (those not already added)
  std::cout << "[Dashboard] Considering " << localAll.size()
            << " local camera(s).\n";
  for (auto ce : localAll) {
    if (knownCamIds_.contains(ce.id)) {
      std::cout << "[Dashboard] Skipping duplicate (local): " << ce.id << '\n';
      continue;
    }
    addCamera(ce.url, ce.id, ce.w, ce.h);
    knownCamIds_.insert(ce.id);

    if (ce.x >= 0 && ce.y >= 0) {
      auto cw = camWidgets_.value(ce.id);
      if (cw.wrapper) {
        std::cout << "[Dashboard] Restoring " << ce.id
                  << " to local=" << QString("(%1,%2)").arg(ce.x).arg(ce.y)
                  << '\n';

        cw.wrapper->move(QPoint(ce.x, ce.y));

        std::cout << "[Dashboard] After move wrapper local="
                  << QString("(%1,%2)")
                         .arg(cw.wrapper->pos().x())
                         .arg(cw.wrapper->pos().y())
                  << " global="
                  << QString("(%1,%2)")
                         .arg(cw.wrapper->mapToGlobal(QPoint(0, 0)).x())
                         .arg(cw.wrapper->mapToGlobal(QPoint(0, 0)).y())
                  << '\n';

        // Reposition overlay immediately (only if overlay exists)
        if (cw.overlay) {
          repositionOverlay(cw.wrapper, cw.overlay);
        }

        // Also defer once: lets Qt settle sizes/positions if this runs during
        // startup
        QTimer::singleShot(0, this, [this, id = ce.id]() {
          auto cw2 = camWidgets_.value(id);
          if (!cw2.wrapper)
            return;

          std::cout << "[Dashboard] Deferred check for " << id
                    << " wrapper local="
                    << QString("(%1,%2)")
                           .arg(cw2.wrapper->pos().x())
                           .arg(cw2.wrapper->pos().y())
                    << " global="
                    << QString("(%1,%2)")
                           .arg(cw2.wrapper->mapToGlobal(QPoint(0, 0)).x())
                           .arg(cw2.wrapper->mapToGlobal(QPoint(0, 0)).y())
                    << '\n';

          // Only reposition overlay if it exists (not null for WSL)
          if (cw2.overlay) {
            repositionOverlay(cw2.wrapper, cw2.overlay);
          }
        });
      } else {
        std::cout << "[Dashboard] WARN: wrapper missing for " << ce.id << '\n';
      }
    } else
      std::cout << "[Dashboard] No coordinates to restore for " << ce.id
                << std::endl;

    added.push_back(ce);
  }

  // 3) Persist the newly added set (includes x/y if applied)
  saveCamerasJson("config/cameras.json", added);

  std::cout << "[Dashboard] ==== Initial camera load done. Added: "
            << added.size() << " ====\n";
}

void Dashboard::saveSingleCameraGeometry(const QString &id, const QPoint &pos,
                                         const QSize &size) {
  // Load existing file (if any)
  QFile f(camerasJsonPath());
  QJsonObject root;
  if (f.open(QIODevice::ReadOnly)) {
    root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
  }

  // Convert current list to a map for easy update
  QJsonArray arr = root.value("cameras").toArray();
  QMap<QString, int> indexById;
  for (int i = 0; i < arr.size(); ++i) {
    const auto obj = arr[i].toObject();
    indexById.insert(obj.value("id").toString(), i);
  }

  QJsonObject entry;
  if (indexById.contains(id)) {
    entry = arr[indexById[id]].toObject();
  } else {
    // New entry - get URL from camWidgets_
    entry["id"] = id;
    if (auto cw = camWidgets_.value(id); !cw.url.isEmpty()) {
      entry["url"] = cw.url;
      std::cout << "[Dashboard] Added missing URL to new entry: " << cw.url.toStdString() << std::endl;
    } else {
      std::cout << "[Dashboard] WARNING: No URL found for camera: " << id.toStdString() << std::endl;
    }
  }

  // Update geometry (keep url if present)
  entry["x"] = pos.x();
  entry["y"] = pos.y();
  entry["w"] = size.width();
  entry["h"] = size.height();

  if (indexById.contains(id)) {
    arr[indexById[id]] = entry;
  } else {
    arr.append(entry);
  }

  root["cameras"] = arr;

  // Save atomically
  QSaveFile sf(camerasJsonPath());
  if (sf.open(QIODevice::WriteOnly)) {
    sf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    sf.commit();
  }

  std::cout << "[Dashboard] Saved geometry for " << id << " pos=(" << pos.x()
            << "," << pos.y() << ") size=(" << size.width() << "x"
            << size.height() << ")\n";
}

void Dashboard::saveLayoutToDisk() {
  for (const auto &cw : camWidgets_) {
    if (!cw.wrapper)
      continue;
    saveSingleCameraGeometry(cw.id, cw.wrapper->pos(), cw.wrapper->size());
  }
  std::cout << "[Dashboard] Layout saved for " << camWidgets_.size()
            << " camera(s)\n";
}

QString Dashboard::settingsPath() {
  QString base = QCoreApplication::applicationDirPath() + "/config";
  QDir().mkpath(base);
  return base + "/settings.json";
}

void Dashboard::restoreWindowFromSettings() {
  QFile f(settingsPath());
  if (!f.open(QIODevice::ReadOnly)) {
    // No settings yet: keep your current default behavior
    if (ClientSettings::DASHBOARD_MAXIMIZE)
      showMaximized();
    std::cout << "[DashWin] no settings file; using defaults\n";
    return;
  }

  const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
  f.close();

  if (!root.contains("window") || !root["window"].isObject()) {
    if (ClientSettings::DASHBOARD_MAXIMIZE)
      showMaximized();
    std::cout << "[DashWin] no 'window' section; using defaults\n";
    return;
  }

  const QJsonObject win = root["window"].toObject();
  const bool maximized = win.value("maximized").toBool(false);
  menuVisible = win.value("menuVisible")
                    .toBool(true); // we have this bool in class already

  QRect goal;
  if (win.contains("geometry") && win["geometry"].isArray()) {
    const QJsonArray a = win["geometry"].toArray();
    if (a.size() == 4) {
      const int x = a.at(0).toInt();
      const int y = a.at(1).toInt();
      const int w = a.at(2).toInt();
      const int h = a.at(3).toInt();
      goal = QRect(x, y, w, h);
    }
  } else {
    // Fallback separate keys (optional)
    const int x = win.value("x").toInt(100);
    const int y = win.value("y").toInt(100);
    const int w = win.value("w").toInt(900);
    const int h = win.value("h").toInt(600);
    goal = QRect(x, y, w, h);
  }

  if (goal.isValid()) {
    // Keep geometry on-screen if user changed monitors
    auto scr = QGuiApplication::screenAt(goal.center());
    if (!scr)
      scr = QGuiApplication::primaryScreen();
    QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
    if (!avail.contains(goal.topLeft()) ||
        !avail.contains(goal.bottomRight())) {
      goal.moveTopLeft(avail.topLeft()); // simple clamp; can be smarter
    }

    setGeometry(goal);
    std::cout << "[DashWin] restored geometry: (" << goal.x() << "," << goal.y()
              << ") " << goal.width() << "x" << goal.height() << "\n";
  }

  if (maximized) {
    setWindowState(windowState() | Qt::WindowMaximized);
    std::cout << "[DashWin] restored maximized=true\n";
  }

  // Apply menu visibility last (after the window exists and geometry is set)
  if (QMenuBar *mb = QMainWindow::menuBar())
    mb->setVisible(menuVisible);
  std::cout << "[DashWin] restored menuVisible=" << (menuVisible ? "1" : "0")
            << "\n";
}

void Dashboard::saveWindowToSettings() {
  // Capture normal geometry when maximized, so we restore a sensible size
  const bool isMax = isMaximized();
  const QRect g = isMax ? normalGeometry() : geometry();

  // Load existing root, update only "window"
  QFile f(settingsPath());
  QJsonObject root;
  if (f.open(QIODevice::ReadOnly)) {
    root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
  }

  QJsonObject win;
  QJsonArray geom;
  geom.append(g.x());
  geom.append(g.y());
  geom.append(g.width());
  geom.append(g.height());
  win["geometry"] = geom;
  win["maximized"] = isMax;
  win["menuVisible"] = menuVisible;

  root["window"] = win;

  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cout << "[DashWin] FAILED writing settings\n";
    return;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.close();

  std::cout << "[DashWin] saved geometry: (" << g.x() << "," << g.y() << ") "
            << g.width() << "x" << g.height()
            << " maximized=" << (isMax ? "1" : "0")
            << " menuVisible=" << (menuVisible ? "1" : "0") << "\n";
}

void Dashboard::closeEvent(QCloseEvent *e) {
  saveWindowToSettings();
  QMainWindow::closeEvent(e);
}
