#pragma once
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>
#include <QComboBox>

// Custom QLabel for drawing rectangles
class DrawableImageLabel : public QLabel {
  Q_OBJECT
public:
  explicit DrawableImageLabel(QWidget* parent = nullptr);
  void setDrawingMode(bool enabled) { drawingMode_ = enabled; }
  bool isDrawingMode() const { return drawingMode_; }
  void setCurrentRotation(float angle) { currentRotation_ = angle; }
  float getCurrentRotation() const { return currentRotation_; }
  void setCurrentRect(const QRect& rect) { currentRect_ = rect; }
  QRect getCurrentRect() const { return currentRect_; }

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

signals:
  void rectangleDrawn(const QRect& rect);
  void rotationChanged(float angle);

private:
  bool drawingMode_ = false;
  bool drawing_ = false;
  bool dragging_ = false;
  QPoint startPoint_;
  QPoint currentPoint_;
  QPoint dragStartPoint_;
  QPoint dragOffset_;
  float currentRotation_ = 0.0f;
  QRect currentRect_;
};

class MotionFrameDialog : public QDialog {
  Q_OBJECT
public:
  explicit MotionFrameDialog(const QString &cameraId,
                             QWidget *parent = nullptr);

private slots:
  void fetchFrame();
  void handleReply(QNetworkReply *reply);
  void onIntervalChanged(int ms);
  void onScaleChanged(double s);
  void onSetMotionRegionClicked();
  void onRectangleDrawn(const QRect& rect);
  void sendMotionRegionRequest(const QRect& rect, float angle);
  void onRemoveMotionRegionClicked();
  void onClearMotionRegionsClicked();
  void onRotateLeftClicked();
  void onRotateRightClicked();

private:
  void applyImage(const QImage &img);
  void updateFpsCounter();

  DrawableImageLabel *imageLabel_{nullptr};
  QLabel *fpsLabel_{nullptr};
  QSpinBox *intervalSpin_{nullptr};
  QDoubleSpinBox *scaleSpin_{nullptr};
  QPushButton *motionRegionButton_{nullptr};
  QComboBox *regionComboBox_{nullptr};
  QPushButton *removeRegionButton_{nullptr};
  QPushButton *clearRegionsButton_{nullptr};
  QPushButton *rotateLeftButton_{nullptr};
  QPushButton *rotateRightButton_{nullptr};
  QLabel *rotationLabel_{nullptr};

  QTimer timer_;
  QNetworkAccessManager net_;
  QString cameraId_;

  // sizing / scaling
  double scale_ = 1.0;
  int width_ = 0, height_ = 0;

  // fps
  int frameCounter_ = 0;
  QElapsedTimer fpsTick_;
  
  // rotation state
  bool rotationMode_ = false;
  float currentAngle_ = 0.0f;
};
