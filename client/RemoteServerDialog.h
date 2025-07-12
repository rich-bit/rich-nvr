// RemoteServerDialog.h
#pragma once
#include <QDialog>
#include "conversations.h"

class QLineEdit;

class RemoteServerDialog : public QDialog {
  Q_OBJECT
public:
  explicit RemoteServerDialog(QWidget *parent = nullptr);

private:
  QLineEdit *edit;
  void trySave();
};
