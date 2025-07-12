// FileBrowserWindow.h
#ifndef FILEBROWSERWINDOW_H
#define FILEBROWSERWINDOW_H

#include <QMainWindow>
#include <QFileSystemModel>
#include <QTimer>
#include "conversations.h"

class FileBrowserWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit FileBrowserWindow(const QString& startDir = QString(), QWidget* parent = nullptr);

signals:
    void playFileRequested(const QString& filePath);

private slots:
    void chooseDirectory();
    void handleDoubleClick(const QModelIndex& idx);
    void refresh();

private:
    void setDirectory(const QString& dir);

    QFileSystemModel* model;
    QTimer*           refreshTimer;
    QString           currentDir;
};

#endif
