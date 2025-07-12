#include "OverlayBox.h"
#include "MotionFrameDialog.h"
#include "Remote.h"
#include "VideoPlayer.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <nlohmann/json.hpp>
#include "conversations.h"

OverlayBox::OverlayBox(const QString &camId, const QString &camUrl,
                       QWidget *parent)
    : QWidget(parent), id_(camId), url_(camUrl) {
  setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_NoSystemBackground);
  setAttribute(Qt::WA_TransparentForMouseEvents, false);
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  setAutoFillBackground(false);
  setStyleSheet("");
  move(0, 0);
  raise();
  show();
  setupOverlayButtons();

  messageLabel_ = new QLabel(this);
  messageLabel_->setStyleSheet(
      "QLabel { background-color: rgba(0, 0, 0, 160); color: white; padding: "
      "6px 12px; border-radius: 6px; font-weight: bold; }");
  messageLabel_->setAlignment(Qt::AlignCenter);
  messageLabel_->hide();

  messageTimer_ = new QTimer(this);
  messageTimer_->setSingleShot(true);
  connect(messageTimer_, &QTimer::timeout, messageLabel_, &QLabel::hide);
}

void OverlayBox::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) {
    dragStartPos = e->globalPosition().toPoint() - frameGeometry().topLeft();
    isDragging = true;
    update();
  }
}

void OverlayBox::mouseMoveEvent(QMouseEvent *e) {
  if (e->buttons() & Qt::LeftButton) {
    move(e->globalPosition().toPoint() - dragStartPos); // global math
  }
}

void OverlayBox::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  emit resized();
}

void OverlayBox::enterEvent(QEnterEvent *event) {
  Q_UNUSED(event);
  buttonPanel->setVisible(true);
}

void OverlayBox::leaveEvent(QEvent *event) {
  Q_UNUSED(event);
  if (!isDragging)
    buttonPanel->setVisible(false);
}

void OverlayBox::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) {
    isDragging = false;
    update();
    emit droppedAt(frameGeometry().topLeft()); // emit global pos
  }
}

void OverlayBox::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);
  if (!isDragging)
    return;

  QPainter p(this);
  QPen pen(Qt::black, 2, Qt::SolidLine);
  p.setPen(pen);
  p.drawRect(rect().adjusted(1, 1, -2, -2));
}
void OverlayBox::contextMenuEvent(QContextMenuEvent *e) {
  QMenu menu(this);
  menu.addAction("Start Recording");
  menu.addAction("Take Snapshot");
  menu.addSeparator();
  QAction *fullscreenAction = menu.addAction("🔍 Toggle Fullscreen");
  connect(fullscreenAction, &QAction::triggered, this, [this]() {
    std::cout << "[OverlayBox] Opening fullscreen for camera: " << id_.toStdString() << std::endl;
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
void OverlayBox::setupOverlayButtons() {
  buttonPanel = new QWidget(this);
  buttonPanel->setAttribute(Qt::WA_TransparentForMouseEvents, false);
  buttonPanel->setStyleSheet("background-color: rgba(0, 0, 0, 100);");
  buttonPanel->setVisible(false);

  auto *layout = new QHBoxLayout(buttonPanel);
  layout->setContentsMargins(5, 5, 5, 5);
  layout->setSpacing(8);

  QString btnStyle =
      "QPushButton { color: white; background-color: rgba(50,50,50,180); "
      "border: none; padding: 5px 10px; border-radius: 4px; }"
      "QPushButton:hover { background-color: rgba(70,70,70,220); }";

  QPushButton *recordBtn = new QPushButton("Rec", buttonPanel);
  QPushButton *motionBtn = new QPushButton("Toggle Motion", buttonPanel);
  QPushButton *frameBtn = new QPushButton("Frame", buttonPanel);
  QPushButton *fsBtn = new QPushButton("Fullscreen", buttonPanel);
  QPushButton *infoBtn = new QPushButton("Info", buttonPanel);

  recordBtn->setStyleSheet(btnStyle);
  motionBtn->setStyleSheet(btnStyle);
  frameBtn->setStyleSheet(btnStyle);
  fsBtn->setStyleSheet(btnStyle);
  infoBtn->setStyleSheet(btnStyle);

  layout->addWidget(recordBtn);
  layout->addWidget(motionBtn);
  layout->addWidget(frameBtn);
  layout->addWidget(fsBtn);
  layout->addWidget(infoBtn);

  buttonPanel->setLayout(layout);
  buttonPanel->adjustSize();
  int x = (width() - buttonPanel->width()) / 2;
  int y = height() - buttonPanel->height() - 10;
  buttonPanel->move(x, y); // ✅ Bottom-center

  connect(motionBtn, &QPushButton::clicked, this, [this, motionBtn] {
    motionOn_ = !motionOn_;
    sendMotionUpdate(motionOn_);
  });

  connect(frameBtn, &QPushButton::clicked, this, [this]() {
    auto *dlg = new MotionFrameDialog(id_, this); // id_ is this camera’s ID
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
  });

  connect(this, &OverlayBox::resized, this, [=]() {
    int x = (width() - buttonPanel->width()) / 2;
    int y = height() - buttonPanel->height() - 10;
    buttonPanel->move(x, y);
  });
}

void OverlayBox::sendMotionUpdate(bool enable) {
  nlohmann::json j = {{"id", id_.toStdString()},
                      {"url", url_.toStdString()},
                      {"motion_enabled", enable}};
  QByteArray body = QByteArray::fromStdString(j.dump());

  QUrl url(
      QString("http://%1:8080/UpdateCamera").arg(Remote::getCurrentHost()));

  QNetworkRequest req(url);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

  auto *reply = net_.post(req, body);

  QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply] {
    if (reply->error() == QNetworkReply::NoError) {
      int statusCode =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QByteArray responseBody = reply->readAll();

      QString message = QString::fromUtf8(responseBody).trimmed();
      showTemporaryMessage(message, 2000);

      std::cout << "[Overlay] Motion update OK";
      std::cout << "[Overlay] HTTP status:" << statusCode;
      std::cout << "[Overlay] Response body:"
                << QString::fromUtf8(responseBody);
    } else {
      QString errMsg = reply->errorString();
      showTemporaryMessage("Error: " + errMsg, 2500);
      QByteArray errorBody = reply->readAll();
      std::cout << "[Overlay] Error response body:"
                << QString::fromUtf8(errorBody);
    }
    reply->deleteLater();
  });
}

void OverlayBox::deleteCamera() {
  if (id_.isEmpty()) {
    std::cout << "[OverlayBox] No camera ID set for deletion" << std::endl;
    return;
  }

  QUrl reqUrl(QString("http://%1:8080/remove_camera").arg(Remote::getCurrentHost()));
  QNetworkRequest req(reqUrl);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

  // Prepare form data with camera name
  QString formData = QString("name=%1").arg(id_);
  QByteArray body = formData.toUtf8();

  auto *reply = net_.post(req, body);
  connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
    if (reply->error() == QNetworkReply::NoError) {
      int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      QString message = QString::fromUtf8(reply->readAll()).trimmed();
      std::cout << "[OverlayBox] Camera deletion OK: " << statusCode 
                << " " << message.toStdString() << std::endl;
      
      emit cameraDeleted(id_);
    } else {
      QString errMsg = reply->errorString();
      QByteArray errorBody = reply->readAll();
      std::cout << "[OverlayBox] Camera deletion error: " << errMsg.toStdString() << std::endl;
      std::cout << "[OverlayBox] Error body: " << QString::fromUtf8(errorBody).toStdString() << std::endl;
    }
    reply->deleteLater();
  });
}

void OverlayBox::showTemporaryMessage(const QString &msg, int durationMs) {
  if (!messageLabel_) {
    messageLabel_ = new QLabel(this);
    messageLabel_->setStyleSheet(
        "color: white; background-color: rgba(0, 0, 0, 180); padding: 6px; "
        "border-radius: 5px; font-weight: bold;");
    messageLabel_->setAlignment(Qt::AlignCenter);
    messageLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  }

  messageLabel_->setText(msg);
  messageLabel_->adjustSize();

  int x = (width() - messageLabel_->width()) / 2;
  messageLabel_->move(x, 10);
  messageLabel_->show();

  auto *effect = new QGraphicsOpacityEffect(messageLabel_);
  messageLabel_->setGraphicsEffect(effect);
  auto *anim = new QPropertyAnimation(effect, "opacity");
  anim->setDuration(800);
  anim->setStartValue(1.0);
  anim->setEndValue(0.0);
  anim->setEasingCurve(QEasingCurve::OutQuad);

  QTimer::singleShot(durationMs, this, [=] {
    anim->start(QAbstractAnimation::DeleteWhenStopped);
    connect(anim, &QPropertyAnimation::finished, messageLabel_, [=]() {
      messageLabel_->hide();
      effect->deleteLater();
    });
  });
}

void OverlayBox::openFullscreenPlayer() {
  if (url_.isEmpty()) {
    std::cout << "[OverlayBox] No camera URL available for fullscreen" << std::endl;
    return;
  }

  auto *fullscreenPlayer = new VideoPlayer(nullptr); // No parent - independent window
  fullscreenPlayer->setAttribute(Qt::WA_DeleteOnClose); // Auto-delete when closed
  fullscreenPlayer->setWindowTitle(QString("Fullscreen - %1").arg(id_));
  
  fullscreenPlayer->playUri(url_);
  
  fullscreenPlayer->showFullScreen();
  
  std::cout << "[OverlayBox] Opened fullscreen player for " << id_.toStdString() 
            << " URL: " << url_.toStdString() << std::endl;
}
