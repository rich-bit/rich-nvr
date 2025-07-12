// Remote.cpp
#include "Remote.h"
//#include "Log.h"
#include "conversations.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>

namespace {

QString settingsPath() {
  QString base = QCoreApplication::applicationDirPath() + "/config";
  QDir().mkpath(base);
  QString full = base + "/settings.json";
  std::cout << "[REMOTE] settings path is" << full;
  return full;
}

QNetworkAccessManager &mgr() {
  static QNetworkAccessManager m;
  return m;
}

} // namespace

namespace {
QUrl makeBaseUrl(const QString &hostOrUrl) {
  QString s = hostOrUrl.trimmed();
  if (!s.startsWith("http://") && !s.startsWith("https://"))
    s.prepend("http://");
  QUrl u(s);
  if (u.port() == -1)
    u.setPort(8080);
  return u;
}
} // namespace

QString Remote::host = "127.0.0.1";

// Temp host logic
QString Remote::getCurrentHost() {
  return tempHost.isEmpty() ? host : tempHost;
}

void Remote::setTemporaryHost(const QString &h) { tempHost = h; }

void Remote::load() {
  QFile f(settingsPath());
  if (!f.open(QIODevice::ReadOnly))
    return;
  QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
  host = obj.value("server").toString(host);
  std::cout << "[REMOTE] host set to " << host;
}

// Remote.cpp
void Remote::save() {
  std::cout << "[REMOTE] save server address " << host;

  // Load existing JSON (so we don't nuke other keys like "window")
  QFile f(settingsPath());
  QJsonObject root;
  if (f.open(QIODevice::ReadOnly)) {
    root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
  }

  root["server"] = host;

  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    std::cout << "[REMOTE] FAILED opening settings for write\n";
    return;
  }
  f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  f.close();
  std::cout << "[REMOTE] settings.json saved (merged)\n";
}

// Get health status on server
bool Remote::isServerReachable(QByteArray &body) {
  const QString host = getCurrentHost();
  std::cout << "[REMOTE] Will check host " << host << '\n';

  // Try /health first
  QUrl base = makeBaseUrl(host);
  QUrl url = base;
  url.setPath("/health");

  QNetworkReply *rep = mgr().get(QNetworkRequest(url));
  QEventLoop loop;
  QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
  loop.exec();

  int http = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (rep->error() == QNetworkReply::NoError && http == 200) {
    body = rep->readAll();
    rep->deleteLater();
    std::cout << "[REMOTE] /health OK\n";
    return true;
  }
  std::cout << "[REMOTE] /health failed: " << rep->errorString() << " (HTTP "
            << http << ")\n";
  rep->deleteLater();

  return false;
}