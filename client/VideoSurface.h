// VideoSurface.h
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QPainter>


class VideoSurface : public QWidget {
  Q_OBJECT
public:
  explicit VideoSurface(QWidget* parent = nullptr) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
  }

public slots:
  void setFrame(const QImage& img) {
    {
      QMutexLocker lock(&mtx_);
      frame_ = img.copy();
    }
    update();
  }

protected:
  void paintEvent(QPaintEvent*) override {
    QImage img;
    {
      QMutexLocker lock(&mtx_);
      img = frame_;
    }
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (!img.isNull()) {
      // letterbox fit
      QSize s = img.size();
      s.scale(size(), Qt::KeepAspectRatio);
      QRect r(QPoint(0,0), s);
      r.moveCenter(rect().center());
      p.drawImage(r, img);
    }
  }

private:
  QImage frame_;
  QMutex mtx_;
};
