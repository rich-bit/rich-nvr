#pragma once

#include "AddStreamDialog.h"
#include "Settings.h"
#include "conversations.h"
#include <QElapsedTimer>
#include <QMainWindow>
#include <QMenuBar>
#include <QNetworkAccessManager>
#include <QSet>
#include <QTcpSocket>

struct CameraEntry {
  QString id;
  QString url;
  int w = 0;
  int h = 0;
  int x = -1;
  int y = -1;
  QWidget *wrapper = nullptr;
  QWidget *overlay = nullptr;
};

class Dashboard : public QMainWindow {
  Q_OBJECT

public:
  explicit Dashboard(QWidget *parent = nullptr);
  void addCamera(const QString &url, const QString &camID, int width,
                 int height);
  void saveLayoutToDisk();

protected:
  void contextMenuEvent(QContextMenuEvent *event) override;
  bool eventFilter(QObject *obj, QEvent *event) override;
  void closeEvent(QCloseEvent *e) override;

private:
  QHash<QString, CameraEntry> camWidgets_;
  void saveSingleCameraGeometry(const QString &id, const QPoint &pos,
                                const QSize &size);
  void repositionOverlay(QWidget *wrapper, QWidget *overlay);
  void removeCameraFromUI(const QString &cameraID);
  void updateCamerasJsonAfterDelete(const QString &cameraID);

  void onAddStreamTriggered();
  void createMenuBar();
  bool sendAddCameraToServer(const QString &server, const QString &id,
                             const QString &url, bool segment, bool motionFrame,
                             bool liveProxied, const QSize &motionFrameSize,
                             float motionFrameScale, float noiseThreshold,
                             float motionThreshold, int motionMinHits,
                             int motionDecay, float motionArrowScale,
                             float motionArrowThickness);
  bool waitForRtspReady(const QUrl &rtspUrl, int overallTimeoutMs = 5000);

  QSet<QString> knownCamIds_;
  QMenuBar *menuBar = nullptr;
  bool menuVisible = true;
  QLabel *logo = nullptr;
  QNetworkAccessManager manager_;
  Settings settings_;
  void restoreWindowFromSettings();
  void saveWindowToSettings();
  static QString settingsPath(); // Path of settings.json

  // Startup sync
  void loadInitialCameras();
  static QString readServerFromSettingsJson(const QString &path);
  QVector<CameraEntry> fetchServerCameras(const QString &server);
  static QVector<CameraEntry> readLocalCamerasJson(const QString &path);
  static void saveCamerasJson(const QString &path,
                              const QVector<CameraEntry> &cams);
  static void replaceCamerasJson(const QString &path,
                                const QVector<CameraEntry> &cameras);
};
