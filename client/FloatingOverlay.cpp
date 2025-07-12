#include "FloatingOverlay.h"
#include "MotionFrameDialog.h"
#include "Remote.h"
#include "VideoPlayer.h"
//#include "Log.h"
#include <QDebug>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QTimer>
#include "conversations.h"

FloatingOverlay::FloatingOverlay(const QString &camID, const QString &url,
                                 QWidget *parent)
    : QWidget(parent), id_(camID), url_(url) {
  setAttribute(Qt::WA_TranslucentBackground);
  setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);

  auto *layout = new QHBoxLayout(this);
  layout->setSpacing(10);
  layout->setContentsMargins(10, 10, 10, 10);

  auto *rec = new QPushButton("Record", this);
  rec->setStyleSheet("background-color: #444; color: white; border-radius: "
                     "5px; padding: 5px 10px;");
  layout->addWidget(rec);

  // Motion record on/off
  auto *toggleMotionBtn = new QPushButton("Toggle Motion", this);
  toggleMotionBtn->setStyleSheet("background-color: #444; color: white; "
                                 "border-radius: 5px; padding: 5px 10px;");
  layout->addWidget(toggleMotionBtn);

  connect(toggleMotionBtn, &QPushButton::clicked, this, [this]() {
    motionOn_ = !motionOn_;
    sendMotionUpdate(motionOn_); // You could toggle state with a flag later
  });

  auto *motionFrames = new QPushButton("Motion Frames", this);
  motionFrames->setStyleSheet("background-color: #444; color: white; "
                              "border-radius: 5px; padding: 5px 10px;");
  layout->addWidget(motionFrames);

  connect(motionFrames, &QPushButton::clicked, this, [this]() {
    auto *dlg = new MotionFrameDialog(id_, this); // id_ is this camera’s ID
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
  });

  auto *fullscreen = new QPushButton("Fullscreen", this);
  fullscreen->setStyleSheet("background-color: #444; color: white; "
                            "border-radius: 5px; padding: 5px 10px;");
  connect(fullscreen, &QPushButton::clicked, this, [this]() {
    std::cout << "[FloatingOverlay] Opening fullscreen for camera: " << id_.toStdString() << std::endl;
    openFullscreenPlayer();
  });
  layout->addWidget(fullscreen);
  setLayout(layout);
}

void FloatingOverlay::sendMotionUpdate(bool enable) {
  // Build JSON payload
  QJsonObject json;
  json["id"] = id_;
  json["url"] = url_;
  json["motion_enabled"] = enable;
  QByteArray body = QJsonDocument(json).toJson();

  QUrl reqUrl(
      QString("http://%1:8080/UpdateCamera").arg(Remote::getCurrentHost()));
  QNetworkRequest req(reqUrl);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  auto *reply = net_.post(req, body);
  connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      int statusCode =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QString message = QString::fromUtf8(reply->readAll()).trimmed();
      brieflyShowServerMessage(message); // << fade-toast
      std::cout << "[FloatingOverlay] Motion update OK:" << statusCode
                << message;
    } else {
      QString errMsg = reply->errorString();
      QByteArray errorBody = reply->readAll();
      std::cout << "[FloatingOverlay] Error:" << errMsg;
      std::cout << "[FloatingOverlay] Body:" << QString::fromUtf8(errorBody);
    }
    reply->deleteLater();
  });
}

void FloatingOverlay::brieflyShowServerMessage(const QString &msg,
                                               int durationMs /* = 2000 */) {
  // parent is the VideoWrapper tile
  QWidget *videoWrapper = parentWidget();
  if (!videoWrapper) // should never happen
    return;

  /* ------------ create label once ------------ */
  if (!messageLabel_) {
    messageLabel_ = new QLabel(videoWrapper);
    messageLabel_->setStyleSheet(R"(
            QLabel {
                color: white;
                background-color: rgba(0, 0, 0, 160);   /* translucent */
                padding: 6px 12px;
                border: 0;            /* square edges, no white halo */
                font-weight: bold;
            })");
    messageLabel_->setAlignment(Qt::AlignCenter);
    messageLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  /* ------------ update text & position ------------ */
  messageLabel_->setText(msg);
  messageLabel_->adjustSize();

  const int x = (videoWrapper->width() - messageLabel_->width()) / 2;
  const int y = 10; // 10 px from the top of the tile
  messageLabel_->move(x, y);
  messageLabel_->show();
  messageLabel_->raise();

  /* ------------ hide after timeout ------------ */
  QTimer::singleShot(durationMs, messageLabel_,
                     [lbl = messageLabel_]() { lbl->hide(); });
}

void FloatingOverlay::openFullscreenPlayer() {
  if (url_.isEmpty()) {
    std::cout << "[FloatingOverlay] No camera URL available for fullscreen" << std::endl;
    return;
  }

  // Create a new fullscreen VideoPlayer window
  auto *fullscreenPlayer = new VideoPlayer(nullptr); // No parent - independent window
  fullscreenPlayer->setAttribute(Qt::WA_DeleteOnClose); // Auto-delete when closed
  fullscreenPlayer->setWindowTitle(QString("Fullscreen - %1").arg(id_));
  
  // Start playing the stream
  fullscreenPlayer->playUri(url_);
  
  // Show fullscreen
  fullscreenPlayer->showFullScreen();
  
  std::cout << "[FloatingOverlay] Opened fullscreen player for " << id_.toStdString() 
            << " URL: " << url_.toStdString() << std::endl;
}
