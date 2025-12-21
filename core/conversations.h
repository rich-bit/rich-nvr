#pragma once
#include <QByteArray>
#include <QString>
#include <iostream>

inline std::ostream &operator<<(std::ostream &os, const QString &str) {
  return os << str.toUtf8().constData();
}

inline std::ostream &operator<<(std::ostream &os, const QByteArray &ba) {
  return os << ba.constData();
}
