#pragma once
#include <QDialog>
#include <QTextEdit>
#include "conversations.h"

class CamerasInfoDialog : public QDialog {
  Q_OBJECT
public:
  explicit CamerasInfoDialog(QWidget *parent = nullptr);
  void setText(const QString &text);
  void setRefreshInterval(int milliseconds);

private:
  void load();
  QTimer *refreshTimer;
  QTextEdit *infoBox;
};