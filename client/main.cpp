#include "Dashboard.h"
#include "FileBrowserWindow.h"
#include "GuiHelpers.h"
#include "ClientSettings.h"
#include "Remote.h"
#include "Settings.h"
#include "VideoPlayer.h"
#include "conversations.h"
#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QThread>
#include <gst/gst.h>
#include <iostream>


static void forceX11() {
  // Force Qt to X11
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("XDG_SESSION_TYPE", "x11");

  // Kill Wayland hints so our helpers don’t get confused
  qunsetenv("WAYLAND_DISPLAY");
  // Optional: also nuke XDG_RUNTIME_DIR so Wayland probing won’t succeed

  // If DISPLAY missing (rare), set WSLg default
  if (qEnvironmentVariableIsEmpty("DISPLAY"))
    qputenv("DISPLAY", ":0");
}

int main(int argc, char *argv[]) {

  Settings settings("settings.json");

  gst_init(&argc, &argv);
  std::cout << "[GST] gst_init done";

  std::cout << "[LOG] logging system initialised";
  
  
  QApplication app(argc, argv);
  std::cout << "[QT] QApplication created";

  Dashboard dashboard;
  std::cout << "[GUI] Dashboard created";

  // -------- construct windows -------------------
  FileBrowserWindow browser("videos/camera_1");
  std::cout << "[GUI] FileBrowserWindow created";

  VideoPlayer player;
  std::cout << "[GUI] VideoPlayer created";

  QObject::connect(&browser, &FileBrowserWindow::playFileRequested, &player,
                   &VideoPlayer::playUri);
  std::cout << "[GUI] Connected FileBrowser ➝ VideoPlayer";

  // -------- show windows -------------------------
  dashboard.show();
  if (ClientSettings::START_WITH_FILEBROWSER)
    browser.show();
  if (ClientSettings::START_WITH_VIDEOPLAYER)
    player.show();

  std::cout << "[GUI] All windows shown";

  QObject::connect(&app, &QCoreApplication::aboutToQuit, &dashboard,
                   [&]() { dashboard.saveLayoutToDisk(); });


  // -------- exec ---------------------------------
  const int rc = app.exec();

  std::cout << "[QT] QApplication exited with" << rc;
  return rc;
}
