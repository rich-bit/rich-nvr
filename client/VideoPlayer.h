#pragma once

#include <QMainWindow>
#include <QWidget>
#include <gst/gst.h>
#include "conversations.h"
#include <QTimer>


class VideoSurface;

class VideoPlayer : public QMainWindow {
  Q_OBJECT

public:
  VideoPlayer(QWidget *parent = nullptr);
  void playUri(const QString &uri);
  ~VideoPlayer();
  
private:
  void handleVideoFrame(GstSample* sample);

private:
  GstElement *pipeline = nullptr;
  GstState lastPipelineState = GST_STATE_NULL;
  QWidget *videoWidget = nullptr;
  QTimer *busTimer_ = nullptr;
  GstBus *bus_ = nullptr;
  QString lastUri_;
  int playRetries_ = 0;
  VideoSurface* surface_ = nullptr;   // only used on Wayland
  GstElement*   appSink_ = nullptr;   // appsink handle


protected:
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
public slots:
  void toggleFullscreen();
  void togglePause();
  void seekRelative(gint seconds);
};
