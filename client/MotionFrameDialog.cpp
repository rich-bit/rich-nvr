#include "MotionFrameDialog.h"
//#include "Log.h"
#include "ClientSettings.h"
#include "Remote.h"
#include "conversations.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QNetworkRequest>
#include <QPixmap>
#include <QVBoxLayout>
#include <QtGlobal>
#include <QMessageBox>
#include <QUrlQuery>
#include <nlohmann/json.hpp>

// DrawableImageLabel implementation
DrawableImageLabel::DrawableImageLabel(QWidget* parent) : QLabel(parent) {
  setMouseTracking(true);
}

void DrawableImageLabel::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    if (drawingMode_) {
      // Drawing mode: start drawing a new rectangle
      drawing_ = true;
      startPoint_ = event->pos();
      currentPoint_ = event->pos();
      setCursor(Qt::CrossCursor);
      update();
    } else if (!currentRect_.isEmpty()) {
      // Check if clicking inside existing rectangle for dragging
      QRect checkRect = currentRect_;
      
      // For rotated rectangles, we need to do more complex hit testing
      if (currentRotation_ != 0.0f) {
        // For simplicity, use bounding rectangle for hit testing
        QPointF center = checkRect.center();
        QTransform transform;
        transform.translate(center.x(), center.y());
        transform.rotate(-currentRotation_); // Inverse rotation
        transform.translate(-center.x(), -center.y());
        
        QPointF mappedPoint = transform.map(QPointF(event->pos()));
        if (checkRect.contains(mappedPoint.toPoint())) {
          dragging_ = true;
          dragStartPoint_ = event->pos();
          dragOffset_ = event->pos() - checkRect.topLeft();
          setCursor(Qt::SizeAllCursor);
        }
      } else {
        // Simple rectangle hit testing
        if (checkRect.contains(event->pos())) {
          dragging_ = true;
          dragStartPoint_ = event->pos();
          dragOffset_ = event->pos() - checkRect.topLeft();
          setCursor(Qt::SizeAllCursor);
        }
      }
      update();
    }
  }
  QLabel::mousePressEvent(event);
}

void DrawableImageLabel::mouseMoveEvent(QMouseEvent* event) {
  if (drawing_) {
    currentPoint_ = event->pos();
    update();
  } else if (dragging_) {
    // Calculate new position for the rectangle
    QPoint newTopLeft = event->pos() - dragOffset_;
    
    // Keep rectangle within bounds
    QRect bounds = rect();
    newTopLeft.setX(qMax(0, qMin(newTopLeft.x(), bounds.width() - currentRect_.width())));
    newTopLeft.setY(qMax(0, qMin(newTopLeft.y(), bounds.height() - currentRect_.height())));
    
    currentRect_.moveTo(newTopLeft);
    update();
  } else if (!currentRect_.isEmpty()) {
    // Show appropriate cursor when hovering
    bool insideRect = false;
    
    if (currentRotation_ != 0.0f) {
      // Rotated rectangle hover detection
      QPointF center = currentRect_.center();
      QTransform transform;
      transform.translate(center.x(), center.y());
      transform.rotate(-currentRotation_);
      transform.translate(-center.x(), -center.y());
      
      QPointF mappedPoint = transform.map(QPointF(event->pos()));
      insideRect = currentRect_.contains(mappedPoint.toPoint());
    } else {
      insideRect = currentRect_.contains(event->pos());
    }
    
    setCursor(insideRect ? Qt::SizeAllCursor : Qt::ArrowCursor);
  }
  
  QLabel::mouseMoveEvent(event);
}

void DrawableImageLabel::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    if (drawing_) {
      drawing_ = false;
      setCursor(Qt::ArrowCursor);
      
      QRect rect(startPoint_, currentPoint_);
      rect = rect.normalized(); // Ensure positive width/height
      
      if (rect.width() > 5 && rect.height() > 5) { // Minimum size threshold
        emit rectangleDrawn(rect);
      }
      
      update();
    } else if (dragging_) {
      dragging_ = false;
      setCursor(Qt::ArrowCursor);
      update();
    }
  }
  QLabel::mouseReleaseEvent(event);
}

void DrawableImageLabel::paintEvent(QPaintEvent* event) {
  QLabel::paintEvent(event);
  
  if (drawing_) {
    QPainter painter(this);
    painter.setPen(QPen(Qt::red, 2, Qt::DashLine));
    
    if (currentRotation_ == 0.0f) {
      // Draw regular rectangle for no rotation
      painter.drawRect(QRect(startPoint_, currentPoint_).normalized());
    } else {
      // Draw rotated rectangle
      QRect rect = QRect(startPoint_, currentPoint_).normalized();
      QPointF center = rect.center();
      
      painter.save();
      painter.translate(center);
      painter.rotate(currentRotation_);
      painter.translate(-center);
      painter.drawRect(rect);
      painter.restore();
    }
  } else if (!currentRect_.isEmpty()) {
    // Draw the current rectangle (being rotated/dragged)
    QPainter painter(this);
    
    // Different appearance when dragging vs rotating
    if (dragging_) {
      painter.setPen(QPen(Qt::green, 3, Qt::SolidLine));
      painter.setBrush(QBrush(Qt::green, Qt::DiagCrossPattern));
    } else {
      painter.setPen(QPen(Qt::blue, 2, Qt::SolidLine));
    }
    
    if (currentRotation_ == 0.0f) {
      painter.drawRect(currentRect_);
    } else {
      QPointF center = currentRect_.center();
      painter.save();
      painter.translate(center);
      painter.rotate(currentRotation_);
      painter.translate(-center);
      painter.drawRect(currentRect_);
      painter.restore();
    }
  }
}

MotionFrameDialog::MotionFrameDialog(const QString &cameraId, QWidget *parent)
    : QDialog(parent), imageLabel_(new DrawableImageLabel(this)),
      fpsLabel_(new QLabel(tr("FPS: 0"), this)),
      intervalSpin_(new QSpinBox(this)), scaleSpin_(new QDoubleSpinBox(this)),
      motionRegionButton_(new QPushButton(tr("Set Motion Region"), this)),
      regionComboBox_(new QComboBox(this)),
      removeRegionButton_(new QPushButton(tr("Remove Region"), this)),
      clearRegionsButton_(new QPushButton(tr("Clear All Regions"), this)),
      rotateLeftButton_(new QPushButton(tr("↺ Left"), this)),
      rotateRightButton_(new QPushButton(tr("↻ Right"), this)),
      rotationLabel_(new QLabel(tr("0°"), this)),
      cameraId_(cameraId) {

  setWindowTitle(tr("Motion Frame – %1").arg(cameraId_));
  imageLabel_->setAlignment(Qt::AlignCenter);
  imageLabel_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

  // --- Controls row ---
  intervalSpin_->setRange(10, 10'000);
  intervalSpin_->setValue(ClientSettings::motionFramePollIntervalMs);
  intervalSpin_->setSuffix(" ms");
  connect(intervalSpin_, qOverload<int>(&QSpinBox::valueChanged), this,
          &MotionFrameDialog::onIntervalChanged);

  scaleSpin_->setDecimals(2);
  scaleSpin_->setRange(0.10, 1.00);
  scaleSpin_->setSingleStep(0.05);
  scaleSpin_->setValue(1.00);
  connect(scaleSpin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          &MotionFrameDialog::onScaleChanged);

  fpsLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  fpsLabel_->setMinimumWidth(80);

  // Setup motion region controls
  regionComboBox_->setMinimumWidth(100);
  regionComboBox_->addItem("Select Region...", -1);
  // Add sample regions 1-10
  for (int i = 1; i <= 10; ++i) {
    regionComboBox_->addItem(tr("Region %1").arg(i), i);
  }

  // Setup rotation controls (initially hidden)
  rotateLeftButton_->setVisible(false);
  rotateRightButton_->setVisible(false);
  rotationLabel_->setVisible(false);
  rotationLabel_->setAlignment(Qt::AlignCenter);
  rotationLabel_->setMinimumWidth(30);

  auto *controls = new QWidget(this);
  auto *controlsLayout = new QHBoxLayout(controls);
  controlsLayout->setContentsMargins(0, 0, 0, 0);
  controlsLayout->addWidget(new QLabel("Update:", this));
  controlsLayout->addWidget(intervalSpin_);
  controlsLayout->addSpacing(12);
  controlsLayout->addWidget(new QLabel("Scale:", this));
  controlsLayout->addWidget(scaleSpin_);
  controlsLayout->addSpacing(12);
  controlsLayout->addWidget(motionRegionButton_);
  controlsLayout->addSpacing(8);
  controlsLayout->addWidget(rotateLeftButton_);
  controlsLayout->addWidget(rotationLabel_);
  controlsLayout->addWidget(rotateRightButton_);
  controlsLayout->addStretch();
  controlsLayout->addWidget(fpsLabel_);

  // Motion region management controls (second row)
  auto *regionControls = new QWidget(this);
  auto *regionLayout = new QHBoxLayout(regionControls);
  regionLayout->setContentsMargins(0, 0, 0, 0);
  regionLayout->addWidget(new QLabel("Regions:", this));
  regionLayout->addWidget(regionComboBox_);
  regionLayout->addWidget(removeRegionButton_);
  regionLayout->addWidget(clearRegionsButton_);
  regionLayout->addStretch();

  // --- Main layout ---
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(controls);
  layout->addWidget(regionControls);
  layout->addWidget(imageLabel_);
  setLayout(layout);

  // Start reasonable default size; dialog will grow to image later
  resize(640, 360);

  connect(&timer_, &QTimer::timeout, this, &MotionFrameDialog::fetchFrame);
  connect(&net_, &QNetworkAccessManager::finished, this,
          &MotionFrameDialog::handleReply);
  connect(motionRegionButton_, &QPushButton::clicked, this,
          &MotionFrameDialog::onSetMotionRegionClicked);
  connect(imageLabel_, &DrawableImageLabel::rectangleDrawn, this,
          &MotionFrameDialog::onRectangleDrawn);
  connect(removeRegionButton_, &QPushButton::clicked, this,
          &MotionFrameDialog::onRemoveMotionRegionClicked);
  connect(clearRegionsButton_, &QPushButton::clicked, this,
          &MotionFrameDialog::onClearMotionRegionsClicked);
  connect(rotateLeftButton_, &QPushButton::clicked, this,
          &MotionFrameDialog::onRotateLeftClicked);
  connect(rotateRightButton_, &QPushButton::clicked, this,
          &MotionFrameDialog::onRotateRightClicked);

  // Kick it off
  fpsTick_.start();
  timer_.start(intervalSpin_->value());
  fetchFrame();
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
void MotionFrameDialog::onIntervalChanged(int ms) { timer_.setInterval(ms); }

void MotionFrameDialog::onScaleChanged(double s) {
  scale_ = s;

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  // Qt 5.15+ (incl. Qt 6) – get by value
  QPixmap pm = imageLabel_->pixmap(Qt::ReturnByValue);
  if (!pm.isNull()) {
    applyImage(pm.toImage());
  }
#else
  // Older Qt 5 – pointer API
  if (const QPixmap *pm = imageLabel_->pixmap()) {
    if (!pm->isNull()) {
      applyImage(pm->toImage());
    }
  }
#endif
}

void MotionFrameDialog::fetchFrame() {
  // cache-buster ensures fresh image every poll
  QUrl url(QString("http://%1:8080/motion_frame?name=%2&ts=%3")
               .arg(Remote::getCurrentHost(), cameraId_)
               .arg(QDateTime::currentMSecsSinceEpoch()));

  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                   QNetworkRequest::AlwaysNetwork);
  req.setRawHeader("Cache-Control", "no-cache");
  req.setRawHeader("Pragma", "no-cache");
  net_.get(req); // async → handleReply()
}

void MotionFrameDialog::handleReply(QNetworkReply *reply) {
  reply->deleteLater();

  if (reply->error() != QNetworkReply::NoError) {
    imageLabel_->setText(tr("No motion frame available."));
    return;
  }

  const auto ctVar = reply->header(QNetworkRequest::ContentTypeHeader);
  const auto ct = ctVar.isValid() ? ctVar.toString() : QString();
  if (!ct.startsWith("image/") && !ct.isEmpty()) {
    // Ignore non-image payloads if backend returns something else
    return;
  }

  const QByteArray imageData = reply->readAll();

  QBuffer buffer(const_cast<QByteArray *>(&imageData));
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer); // let Qt auto-detect
  QImage img = reader.read();

  if (img.isNull()) {
    imageLabel_->setText("Failed to load image.");
    return;
  }

  applyImage(img);
  updateFpsCounter();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void MotionFrameDialog::applyImage(const QImage &img) {
  // Only scale down; if scale_ == 1.0, show native pixels
  QSize target = img.size();
  if (scale_ < 1.0) {
    target = QSize(int(target.width() * scale_), int(target.height() * scale_));
  }

  const QPixmap pix = QPixmap::fromImage(
      (scale_ < 1.0)
          ? img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)
          : img);

  imageLabel_->setPixmap(pix);

  // Grow window to fit the scaled image (plus some layout chrome)
  // Compute desired extra based on layout margins
  int ml, mt, mr, mb;
  layout()->getContentsMargins(&ml, &mt, &mr, &mb);
  const int controlsH = layout()->itemAt(0)->widget()->sizeHint().height();

  const QSize desired(pix.width() + ml + mr,
                      pix.height() + mt + mb + controlsH);
  if (desired.width() > width() || desired.height() > height()) {
    resize(desired);
  } else {
    // If smaller, don't shrink aggressively; keep user choice
    // (remove this branch to auto-shrink as well)
  }
}

void MotionFrameDialog::updateFpsCounter() {
  ++frameCounter_;
  if (fpsTick_.elapsed() >= 1000) {
    fpsLabel_->setText(QString("FPS: %1").arg(frameCounter_));
    frameCounter_ = 0;
    fpsTick_.restart();
  }
}

void MotionFrameDialog::onSetMotionRegionClicked() {
  bool isDrawing = imageLabel_->isDrawingMode();
  imageLabel_->setDrawingMode(!isDrawing);
  rotationMode_ = false;
  currentAngle_ = 0.0f;
  
  if (!isDrawing) {
    motionRegionButton_->setText(tr("Cancel Drawing"));
    motionRegionButton_->setStyleSheet("QPushButton { background-color: #ff6b6b; color: white; }");
    imageLabel_->setCursor(Qt::CrossCursor);
    QMessageBox::information(this, tr("Draw Motion Region"), 
                            tr("Click and drag on the image to draw a rectangle for the motion region."));
  } else {
    motionRegionButton_->setText(tr("Set Motion Region"));
    motionRegionButton_->setStyleSheet("");
    imageLabel_->setCursor(Qt::ArrowCursor);
    // Hide rotation controls
    rotateLeftButton_->setVisible(false);
    rotateRightButton_->setVisible(false);
    rotationLabel_->setVisible(false);
    imageLabel_->setCurrentRect(QRect());
    imageLabel_->setCurrentRotation(0.0f);
    imageLabel_->update();
  }
}

void MotionFrameDialog::onRectangleDrawn(const QRect& rect) {
  // Enter rotation mode
  imageLabel_->setDrawingMode(false);
  rotationMode_ = true;
  currentAngle_ = 0.0f;
  
  // Update button states
  motionRegionButton_->setText(tr("Send Region"));
  motionRegionButton_->setStyleSheet("QPushButton { background-color: #28a745; color: white; }");
  
  // Show rotation controls
  rotateLeftButton_->setVisible(true);
  rotateRightButton_->setVisible(true);
  rotationLabel_->setVisible(true);
  rotationLabel_->setText("0°");
  
  // Store the rectangle for rotation
  imageLabel_->setCurrentRect(rect);
  imageLabel_->setCurrentRotation(0.0f);
  imageLabel_->setCursor(Qt::ArrowCursor);
  imageLabel_->update();
  
  // Update the button to send the region (user can now rotate or send directly)
  disconnect(motionRegionButton_, &QPushButton::clicked, this, &MotionFrameDialog::onSetMotionRegionClicked);
  connect(motionRegionButton_, &QPushButton::clicked, this, [this]() {
    // Get current rectangle position (may have been dragged)
    QRect currentRect = imageLabel_->getCurrentRect();
    
    // Scale the rectangle coordinates back to original image size
    QRect scaledRect = currentRect;
    if (scale_ < 1.0) {
      scaledRect = QRect(
        int(currentRect.x() / scale_),
        int(currentRect.y() / scale_), 
        int(currentRect.width() / scale_),
        int(currentRect.height() / scale_)
      );
    }
    
    sendMotionRegionRequest(scaledRect, currentAngle_);
    
    // Reset to normal state
    rotationMode_ = false;
    currentAngle_ = 0.0f;
    motionRegionButton_->setText(tr("Set Motion Region"));
    motionRegionButton_->setStyleSheet("");
    rotateLeftButton_->setVisible(false);
    rotateRightButton_->setVisible(false);
    rotationLabel_->setVisible(false);
    imageLabel_->setCurrentRect(QRect());
    imageLabel_->setCurrentRotation(0.0f);
    imageLabel_->update();
    
    // Reconnect original handler
    disconnect(motionRegionButton_, &QPushButton::clicked, nullptr, nullptr);
    connect(motionRegionButton_, &QPushButton::clicked, this, &MotionFrameDialog::onSetMotionRegionClicked);
  });
}

void MotionFrameDialog::sendMotionRegionRequest(const QRect& rect, float angle) {
  QUrl url(QString("http://%1:8080/add_motion_region").arg(Remote::getCurrentHost()));
  
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  
  QUrlQuery postData;
  postData.addQueryItem("name", cameraId_);
  postData.addQueryItem("x", QString::number(rect.x()));
  postData.addQueryItem("y", QString::number(rect.y()));
  postData.addQueryItem("w", QString::number(rect.width()));
  postData.addQueryItem("h", QString::number(rect.height()));
  postData.addQueryItem("angle", QString::number(angle, 'f', 1));
  
  QNetworkReply* reply = net_.post(request, postData.toString(QUrl::FullyEncoded).toUtf8());
  
  connect(reply, &QNetworkReply::finished, [this, reply, rect, angle]() {
    reply->deleteLater();
    
    if (reply->error() == QNetworkReply::NoError) {
      QByteArray response = reply->readAll();
      try {
        auto json = nlohmann::json::parse(response.toStdString());
        if (json.contains("success") && json["success"].get<bool>()) {
          int regionId = json.value("region_id", -1);
          float serverAngle = json.value("angle", 0.0f);
          QMessageBox::information(this, tr("Success"), 
                                  tr("Motion region %1 added successfully!\nCoordinates: (%2, %3) Size: %4x%5\nRotation: %6°")
                                  .arg(regionId).arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height()).arg(serverAngle, 0, 'f', 1));
        } else {
          QMessageBox::warning(this, tr("Error"), tr("Failed to add motion region."));
        }
      } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Invalid server response."));
      }
    } else {
      QMessageBox::warning(this, tr("Network Error"), 
                          tr("Failed to send motion region: %1").arg(reply->errorString()));
    }
  });
}

void MotionFrameDialog::onRemoveMotionRegionClicked() {
  int selectedRegionId = regionComboBox_->currentData().toInt();
  if (selectedRegionId <= 0) {
    QMessageBox::warning(this, tr("No Selection"), tr("Please select a motion region to remove."));
    return;
  }
  
  QUrl url(QString("http://%1:8080/remove_motion_region").arg(Remote::getCurrentHost()));
  
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  
  QUrlQuery postData;
  postData.addQueryItem("name", cameraId_);
  postData.addQueryItem("region_id", QString::number(selectedRegionId));
  
  QNetworkReply* reply = net_.post(request, postData.toString(QUrl::FullyEncoded).toUtf8());
  
  connect(reply, &QNetworkReply::finished, [this, reply, selectedRegionId]() {
    reply->deleteLater();
    
    if (reply->error() == QNetworkReply::NoError) {
      QByteArray response = reply->readAll();
      try {
        auto json = nlohmann::json::parse(response.toStdString());
        if (json.contains("success") && json["success"].get<bool>()) {
          QMessageBox::information(this, tr("Success"), 
                                  tr("Motion region %1 removed successfully!").arg(selectedRegionId));
          // Reset combobox to default selection
          regionComboBox_->setCurrentIndex(0);
        } else {
          QMessageBox::warning(this, tr("Error"), tr("Failed to remove motion region."));
        }
      } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Invalid server response."));
      }
    } else {
      QMessageBox::warning(this, tr("Network Error"), 
                          tr("Failed to remove motion region: %1").arg(reply->errorString()));
    }
  });
}

void MotionFrameDialog::onClearMotionRegionsClicked() {
  int ret = QMessageBox::question(this, tr("Confirm Clear"), 
                                 tr("Are you sure you want to clear all motion regions for camera '%1'?")
                                 .arg(cameraId_),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No);
  
  if (ret != QMessageBox::Yes) {
    return;
  }
  
  QUrl url(QString("http://%1:8080/clear_motion_regions").arg(Remote::getCurrentHost()));
  
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  
  QUrlQuery postData;
  postData.addQueryItem("name", cameraId_);
  
  QNetworkReply* reply = net_.post(request, postData.toString(QUrl::FullyEncoded).toUtf8());
  
  connect(reply, &QNetworkReply::finished, [this, reply]() {
    reply->deleteLater();
    
    if (reply->error() == QNetworkReply::NoError) {
      QByteArray response = reply->readAll();
      try {
        auto json = nlohmann::json::parse(response.toStdString());
        if (json.contains("success") && json["success"].get<bool>()) {
          QMessageBox::information(this, tr("Success"), 
                                  tr("All motion regions cleared successfully!"));
          // Reset combobox to default selection
          regionComboBox_->setCurrentIndex(0);
        } else {
          QMessageBox::warning(this, tr("Error"), tr("Failed to clear motion regions."));
        }
      } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Error"), tr("Invalid server response."));
      }
    } else {
      QMessageBox::warning(this, tr("Network Error"), 
                          tr("Failed to clear motion regions: %1").arg(reply->errorString()));
    }
  });
}

void MotionFrameDialog::onRotateLeftClicked() {
  if (rotationMode_) {
    currentAngle_ -= 15.0f;
    if (currentAngle_ < 0.0f) currentAngle_ += 360.0f;
    
    imageLabel_->setCurrentRotation(currentAngle_);
    rotationLabel_->setText(QString("%1°").arg(currentAngle_, 0, 'f', 0));
    imageLabel_->update();
  }
}

void MotionFrameDialog::onRotateRightClicked() {
  if (rotationMode_) {
    currentAngle_ += 15.0f;
    if (currentAngle_ >= 360.0f) currentAngle_ -= 360.0f;
    
    imageLabel_->setCurrentRotation(currentAngle_);
    rotationLabel_->setText(QString("%1°").arg(currentAngle_, 0, 'f', 0));
    imageLabel_->update();
  }
}

