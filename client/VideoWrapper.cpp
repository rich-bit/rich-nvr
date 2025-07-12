#include "VideoWrapper.h"
#include "conversations.h"
#include "MotionFrameDialog.h"
#include "Remote.h"
#include "VideoPlayer.h"
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrl>
#include <iostream>

VideoWrapper::VideoWrapper(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setAttribute(Qt::WA_StyledBackground, true);
}

void VideoWrapper::setVideoWidget(QWidget *video) {
  video->setParent(this);
  video->setFixedSize(size());
  video->move(0, 0);
  video->show();
}
void VideoWrapper::addOverlayButtons() {
  auto *overlay = new QWidget(this);
  overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  overlay->setStyleSheet("background: transparent;");
  overlay->setGeometry(10, 10, 200, 40); // Adjust as needed

  auto *layout = new QHBoxLayout(overlay);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(5);

  auto *pauseBtn = new QPushButton("Pause", overlay);
  auto *stopBtn = new QPushButton("Stop", overlay);

  connect(pauseBtn, &QPushButton::clicked, [] { qDebug() << "Pause clicked"; });
  connect(stopBtn, &QPushButton::clicked, [] { qDebug() << "Stop clicked"; });

  layout->addWidget(pauseBtn);
  layout->addWidget(stopBtn);

  overlay->show();
}

void VideoWrapper::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton)
    dragStartPos = e->pos();
}

void VideoWrapper::mouseMoveEvent(QMouseEvent *ev) {
  if (!(ev->buttons() & Qt::LeftButton))
    return;

  const QPoint delta = ev->pos() - dragStartPos;
  QPoint newPos = pos() + delta;
  
  // Prevent extreme negative positions (allow up to -100 for edge cases)
  if (newPos.x() < -100) newPos.setX(-100);
  if (newPos.y() < -100) newPos.setY(-100);
  
  move(newPos);

  emit positionChanged(mapToGlobal(QPoint(0, 0))); // 🔥 emit on move
}

void VideoWrapper::enterEvent(QEnterEvent *ev) {
  emit entered();
  QWidget::enterEvent(ev);
}

void VideoWrapper::leaveEvent(QEvent *ev) {
  emit left();
  QWidget::leaveEvent(ev);
}

void VideoWrapper::setCameraInfo(const QString &camID, const QString &url) {
  cameraID_ = camID;
  cameraURL_ = url;
}

void VideoWrapper::contextMenuEvent(QContextMenuEvent *e) {
  if (cameraID_.isEmpty()) {
    QWidget::contextMenuEvent(e);
    return;
  }

  QMenu menu(this);
  
  // Add Record action (placeholder for now)
  QAction *recordAction = menu.addAction("📹 Record");
  connect(recordAction, &QAction::triggered, this, [this]() {
    std::cout << "[VideoWrapper] Record clicked for camera: " << cameraID_.toStdString() << std::endl;
    // TODO: Implement record functionality
  });

  // Add Toggle Motion action
  QString motionText = motionEnabled_ ? "⏹️ Disable Motion" : "▶️ Enable Motion";
  QAction *motionAction = menu.addAction(motionText);
  connect(motionAction, &QAction::triggered, this, [this]() {
    motionEnabled_ = !motionEnabled_;
    sendMotionUpdate(motionEnabled_);
  });

  // Add Motion Frames action
  QAction *framesAction = menu.addAction("🖼️ Motion Frames");
  connect(framesAction, &QAction::triggered, this, [this]() {
    auto *dlg = new MotionFrameDialog(cameraID_, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
  });

  // Add Fullscreen action
  QAction *fullscreenAction = menu.addAction("🔍 Fullscreen");
  connect(fullscreenAction, &QAction::triggered, this, [this]() {
    std::cout << "[VideoWrapper] Opening fullscreen for camera: " << cameraID_.toStdString() << std::endl;
    openFullscreenPlayer();
  });

  menu.addSeparator();

  // Add Delete video action
  QAction *deleteAction = menu.addAction("🗑️ Delete video");
  connect(deleteAction, &QAction::triggered, this, [this]() {
    deleteCamera();
  });

  menu.exec(e->globalPos());
}

void VideoWrapper::sendMotionUpdate(bool enable) {
  // Network manager as static to avoid recreation
  static QNetworkAccessManager netManager;
  
  // Build JSON payload
  QJsonObject json;
  json["id"] = cameraID_;
  json["url"] = cameraURL_;
  json["motion_enabled"] = enable;
  QByteArray body = QJsonDocument(json).toJson();

  QUrl reqUrl(QString("http://%1:8080/UpdateCamera").arg(Remote::getCurrentHost()));
  QNetworkRequest req(reqUrl);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  auto *reply = netManager.post(req, body);
  connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QString message = QString::fromUtf8(reply->readAll()).trimmed();
      std::cout << "[VideoWrapper] Motion update OK: " << statusCode 
                << " " << message.toStdString() << std::endl;
    } else {
      QString errMsg = reply->errorString();
      QByteArray errorBody = reply->readAll();
      std::cout << "[VideoWrapper] Motion update error: " << errMsg.toStdString() << std::endl;
      std::cout << "[VideoWrapper] Error body: " << QString::fromUtf8(errorBody).toStdString() << std::endl;
    }
    reply->deleteLater();
  });
}

void VideoWrapper::deleteCamera() {
  if (cameraID_.isEmpty()) {
    std::cout << "[VideoWrapper] No camera ID set for deletion" << std::endl;
    return;
  }

  // Network manager as static to avoid recreation
  static QNetworkAccessManager netManager;
  
  QUrl reqUrl(QString("http://%1:8080/remove_camera").arg(Remote::getCurrentHost()));
  QNetworkRequest req(reqUrl);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

  // Prepare form data with camera name
  QString formData = QString("name=%1").arg(cameraID_);
  QByteArray body = formData.toUtf8();

  auto *reply = netManager.post(req, body);
  connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QString message = QString::fromUtf8(reply->readAll()).trimmed();
      std::cout << "[VideoWrapper] Camera deletion OK: " << statusCode 
                << " " << message.toStdString() << std::endl;
      
      // Emit signal for Dashboard to cleanup UI
      emit cameraDeleted(cameraID_);
    } else {
      QString errMsg = reply->errorString();
      QByteArray errorBody = reply->readAll();
      std::cout << "[VideoWrapper] Camera deletion error: " << errMsg.toStdString() << std::endl;
      std::cout << "[VideoWrapper] Error body: " << QString::fromUtf8(errorBody).toStdString() << std::endl;
    }
    reply->deleteLater();
  });
}

void VideoWrapper::openFullscreenPlayer() {
  if (cameraURL_.isEmpty()) {
    std::cout << "[VideoWrapper] No camera URL available for fullscreen" << std::endl;
    return;
  }

  // Create a new fullscreen VideoPlayer window
  auto *fullscreenPlayer = new VideoPlayer(nullptr); // No parent - independent window
  fullscreenPlayer->setAttribute(Qt::WA_DeleteOnClose); // Auto-delete when closed
  fullscreenPlayer->setWindowTitle(QString("Fullscreen - %1").arg(cameraID_));
  
  // Start playing the stream
  fullscreenPlayer->playUri(cameraURL_);
  
  // Show fullscreen
  fullscreenPlayer->showFullScreen();
  
  std::cout << "[VideoWrapper] Opened fullscreen player for " << cameraID_.toStdString() 
            << " URL: " << cameraURL_.toStdString() << std::endl;
}