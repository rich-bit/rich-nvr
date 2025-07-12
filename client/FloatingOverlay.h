#pragma once

#include <QWidget>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include "conversations.h"

class QLabel;
class QPushButton;

class FloatingOverlay : public QWidget {
    Q_OBJECT
public:
    explicit FloatingOverlay(const QString &camID, const QString &url, QWidget *parent = nullptr);

private:
    QString id_;
    QString url_;
    QNetworkAccessManager net_;
    QLabel* messageLabel_ = nullptr;
    bool motionOn_ = false;
    void sendMotionUpdate(bool enable);
    void brieflyShowServerMessage(const QString &msg, int durationMs = 2000);
    void openFullscreenPlayer();

signals:
    void recordClicked();
    void frameClicked();
    void fullscreenClicked();
    void infoClicked();
};
