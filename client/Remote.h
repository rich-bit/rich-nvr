// Remote.h
#pragma once
#include <QByteArray>
#include <QString>
#include "conversations.h"

namespace Remote {

/* ------------- runtime value ------------- */
extern QString host;                     // e.g. "127.0.0.1"
static QString tempHost;                 // Incase we cant use host
void setTemporaryHost(const QString &h); // switch temporarily

/* ------------- load/save ------------- */
void load(); // called once at startup
void save(); // write host → settings.json

/* ------------- connectivity check ------------- */
bool isServerReachable(QByteArray &jsonOut);

QString getCurrentHost();

} // namespace Remote
