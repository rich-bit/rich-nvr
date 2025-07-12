// CamerasInfoDialog.cpp
#include "CamerasInfoDialog.h"
#include "Remote.h"
#include <QEventLoop>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>
#include "conversations.h"

using json = nlohmann::json;

CamerasInfoDialog::CamerasInfoDialog(QWidget *parent)
    : QDialog(parent), infoBox(new QTextEdit(this)),
      refreshTimer(new QTimer(this)) {

  setWindowTitle("Cameras Info");
  setMinimumSize(400, 300);

  auto *layout = new QVBoxLayout(this);
  infoBox->setReadOnly(true);
  layout->addWidget(infoBox);

  auto *intervalLabel = new QLabel("Auto-refresh interval:", this);
  layout->addWidget(intervalLabel);

  auto *intervalBox = new QSpinBox(this);
  intervalBox->setRange(500, 10000); // 0.5s to 10s
  intervalBox->setValue(2000);
  intervalBox->setSuffix(" ms");
  intervalBox->setToolTip("Adjust how often the camera/motion info is updated");
  layout->addWidget(intervalBox);

  connect(intervalBox, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &CamerasInfoDialog::setRefreshInterval);

  // Connect timer
  connect(refreshTimer, &QTimer::timeout, this, &CamerasInfoDialog::load);
  setRefreshInterval(2000); // default 2 seconds

  // First update
  load();

  refreshTimer->start();
}

void CamerasInfoDialog::setRefreshInterval(int milliseconds) {
  refreshTimer->setInterval(milliseconds);
}

void CamerasInfoDialog::setText(const QString &text) { infoBox->setText(text); }

void CamerasInfoDialog::load() {
  QNetworkAccessManager mgr(this);

  auto get = [&](const QString &url) -> QByteArray {
    QNetworkReply *r = mgr.get(QNetworkRequest{QUrl(url)});
    QEventLoop loop;
    connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray data =
        (r->error() == QNetworkReply::NoError) ? r->readAll() : QByteArray();
    r->deleteLater();
    return data;
  };

  QByteArray camRaw =
      get(QString("http://%1:8080/cameras.json").arg(Remote::getCurrentHost()));
  QByteArray motionRaw = get(
      QString("http://%1:8080/status/motion").arg(Remote::getCurrentHost()));

  QString text;

  // cameras.json
  try {
    json cams = json::parse(camRaw.constData());
    text += "Configured Cameras\n=================\n";
    for (const auto &c : cams)
      text += QString("• %1 (%2)\n")
                  .arg(QString::fromStdString(c.value("id", "")),
                       QString::fromStdString(c.value("url", "")));
  } catch (const std::exception &e) {
    text += "Error: Failed to parse cameras.json\n";
    text += "Raw data: " + QString::fromUtf8(camRaw) + "\n";
    text += "Parse error: " + QString::fromStdString(e.what()) + "\n";
  }

  // status/motion
  try {
    json arr = json::parse(motionRaw.constData());
    text += "\nMotion Workers\n=================\n";
    for (const auto &w : arr)
      text += QString("• %1 — %2 (started %3)\n")
                  .arg(QString::fromStdString(w.value("id", "")),
                       w.value("running", false) ? "running" : "stopped",
                       QString::fromStdString(w.value("started", "")));
  } catch (...) {
    text += "Error: Failed to parse status/motion\n";
  }

  infoBox->setPlainText(text);
}
