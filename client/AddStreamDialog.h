#pragma once
#include <QDialog>
#include <QSize>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;

class AddStreamDialog : public QDialog {
  Q_OBJECT
public:
  explicit AddStreamDialog(QWidget *parent = nullptr);

  // Basic
  QString cameraId() const;
  QString streamUrl() const;

  // Flags
  bool segment() const;
  bool motionFrame() const;
  bool liveProxied() const;

  // Size or scale choice
  bool useMotionFrameSize() const; // true => send size; false => send scale
  QSize motionFrameSize() const;   // valid if useMotionFrameSize()==true
  float motionFrameScale() const;  // used if useMotionFrameSize()==false

  // Analysis params
  float noiseThreshold() const;
  float motionThreshold() const;
  int motionMinHits() const;
  int motionDecay() const;
  float motionArrowScale() const;
  float motionArrowThickness() const;
  float dashboardDisplayScale() const;
  QString targetServer() const;

private slots:
  void onCheckResolution();
  void onSaveClicked();
  void onSizeOrScaleToggled();
  bool isValidStream(const QString &uri, int timeoutMs = 3000);

private:
  void setupUi();

  // Widgets
  QLineEdit *idEdit{nullptr};
  QLineEdit *urlEdit{nullptr};

  QCheckBox *segmentCheck{nullptr};
  QCheckBox *motionFrameCheck{nullptr};
  QCheckBox *liveProxiedCheck{nullptr};

  QRadioButton *sizeRadio{nullptr};
  QRadioButton *scaleRadio{nullptr};

  QSpinBox *widthSpin{nullptr};
  QSpinBox *heightSpin{nullptr};
  QDoubleSpinBox *scaleSpin{nullptr};

  QDoubleSpinBox *noiseSpin{nullptr};
  QDoubleSpinBox *motionThreshSpin{nullptr};
  QSpinBox *minHitsSpin{nullptr};
  QSpinBox *decaySpin{nullptr};
  QDoubleSpinBox *arrowScaleSpin{nullptr};
  QDoubleSpinBox *arrowThickSpin{nullptr};

  QLabel *resLabel{nullptr};
  QPushButton *checkResButton{nullptr};
  QPushButton *initiateButton{nullptr};
  QPushButton *saveButton{nullptr};
  QPushButton *cancelButton{nullptr};
  QComboBox *dashboardSizeCombo{nullptr};
  QLineEdit *serverEdit{nullptr};

  // For discoverer
  int videoW{0}, videoH{0};
};
