// FileBrowserWindow.cpp
#include "FileBrowserWindow.h"
#include "Remote.h"
#include "conversations.h"
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QListView>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QShortcut>
#include <QToolBar>

static QStringList videoFilters() {
  // Add more extensions if needed
  return {"*.mp4", "*.mov", "*.mkv", "*.avi"};
}

FileBrowserWindow::FileBrowserWindow(const QString &startDir, QWidget *parent)
    : QMainWindow(parent), model(new QFileSystemModel(this)),
      refreshTimer(new QTimer(this)) {
  auto *view = new QListView(this);
  setCentralWidget(view);

  // Quit application on ctrl + q
  auto *quitShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), this);
  connect(quitShortcut, &QShortcut::activated, qApp, [] {

    QCoreApplication::quit(); // finally close GUI
  });

  // Model setup
  model->setNameFilters(videoFilters());
  model->setNameFilterDisables(false); // hide everything else
  view->setModel(model);

  connect(view, &QListView::doubleClicked, this,
          &FileBrowserWindow::handleDoubleClick);

  // Toolbar
  auto *tb = addToolBar("Controls");
  tb->addAction("Change Folder…", this, &FileBrowserWindow::chooseDirectory);

  refreshTimer->setInterval(2000);
  connect(refreshTimer, &QTimer::timeout, this, &FileBrowserWindow::refresh);
  refreshTimer->start();

  // Start dir
  setDirectory(startDir.isEmpty() ? QDir::homePath() : startDir);
}

void FileBrowserWindow::setDirectory(const QString &dir) {
  currentDir = dir;
  model->setRootPath(dir);
  static_cast<QListView *>(centralWidget())->setRootIndex(model->index(dir));
  setWindowTitle(QString("Videos in %1").arg(dir));
}

void FileBrowserWindow::chooseDirectory() {
  QString dir = QFileDialog::getExistingDirectory(this, "Select video folder",
                                                  currentDir);
  if (!dir.isEmpty())
    setDirectory(dir);
}

void FileBrowserWindow::handleDoubleClick(const QModelIndex &idx) {
  if (!idx.isValid())
    return;
  QString path = model->filePath(idx);
  emit playFileRequested(path);
}

void FileBrowserWindow::refresh() {
  // Quick hack: force the model to rescan by re-assigning the path
  setDirectory(currentDir);
}
