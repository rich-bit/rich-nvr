#pragma once
#include <QWidget>
#include <QPoint>
#include <QContextMenuEvent>
#include "conversations.h"

class VideoWrapper : public QWidget {
  Q_OBJECT
public:
  explicit VideoWrapper(QWidget *parent = nullptr);
  void setVideoWidget(QWidget *video);
  void addOverlayButtons();
  void setCameraInfo(const QString &camID, const QString &url);

protected:
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void enterEvent(QEnterEvent *ev) override;
  void leaveEvent(QEvent *ev) override;
  void contextMenuEvent(QContextMenuEvent *e) override;

signals:
  void positionChanged(const QPoint &globalPos);
  void entered();
  void left();
  void cameraDeleted(const QString &cameraID);

private:
  QPoint dragStartPos;
  QString cameraID_;
  QString cameraURL_;
  bool motionEnabled_ = false;
  
  void sendMotionUpdate(bool enable);
  void deleteCamera();
  void openFullscreenPlayer();
};
