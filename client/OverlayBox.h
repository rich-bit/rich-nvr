#pragma once
#include <QLabel>
#include <QNetworkAccessManager>
#include <QPoint>
#include <QTimer>
#include <QWidget>
#include "conversations.h"

class OverlayBox : public QWidget {
  Q_OBJECT

signals:
  void droppedAt(const QPoint &globalPos);
  void resized();
  void cameraDeleted(const QString &cameraID);

public:
  explicit OverlayBox(const QString &camId, const QString &camUrl,
                      QWidget *parent = nullptr);

protected:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *e);

  void enterEvent(QEnterEvent *event) override;
  void leaveEvent(QEvent *event) override;

private:
  QPoint dragStartPos; // store as QPoint (integer pixels)
  bool isDragging = false;

  QWidget *buttonPanel = nullptr;
  QTimer *hoverTimer = nullptr;

  void setupOverlayButtons();
  void resizeEvent(QResizeEvent *event);
  void showTemporaryMessage(const QString &msg, int durationMs);

  QString id_;
  QString url_;
  bool motionOn_ = false;

  QNetworkAccessManager net_;

  QLabel *messageLabel_ = nullptr;
  QTimer *messageTimer_ = nullptr;

  void sendMotionUpdate(bool enable);
  void deleteCamera();
  void openFullscreenPlayer();
};
