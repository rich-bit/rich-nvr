// RemoteServerDialog.cpp
#include "RemoteServerDialog.h"
#include "GuiHelpers.h"
//#include "Log.h"
#include "Remote.h"
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QVBoxLayout>
#include "conversations.h"

RemoteServerDialog::RemoteServerDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle("Remote Server");
  edit = new QLineEdit(Remote::host, this);

  auto *bb = new QDialogButtonBox(
      QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
  connect(bb, &QDialogButtonBox::accepted, this, &RemoteServerDialog::trySave);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *lay = new QVBoxLayout(this);
  lay->addWidget(edit);
  lay->addWidget(bb);
}

void RemoteServerDialog::trySave() {
  QString candidate = edit->text().trimmed();
  QByteArray probe;
  Remote::host = candidate;

  if (!Remote::isServerReachable(probe)) {
    QMessageBox::warning(this, "Connection failed",
                         "Could not reach server at " + candidate);
    return;
  }

  Remote::save();

  // Dynamically cast parent to Dashboard
  if (auto *dash = qobject_cast<Dashboard *>(parent())) {
    loadCamerasFromServer(probe, *dash);
  } else {
    std::cout << "[REMOTE] Could not cast parent() to Dashboard";
  }

  accept();
}
