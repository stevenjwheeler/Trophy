#include "WindowService.h"
#include <QDebug>
#include <QFile>

namespace GameRecorder {

WindowService::WindowService(QObject *parent) : QObject(parent) {
}

void WindowService::setTrophyWindowId(unsigned long windowId) {
  m_trophyWindowId = windowId;
}

WindowService::~WindowService() {
}

QString WindowService::getWindowTitle(unsigned long windowId) {
  return fetchWindowTitle(windowId);
}

QString WindowService::fetchWindowTitle(unsigned long windowId) {
  Q_UNUSED(windowId)
  return QString();
}

unsigned long WindowService::getPidForWindow(unsigned long windowId) {
  Q_UNUSED(windowId)
  return 0;
}

QString WindowService::getProcessNameForWindow(unsigned long windowId) {
  if (windowId == 0)
    return QString();

  unsigned long pid = getPidForWindow(windowId);
  if (pid == 0)
    return QString();

  QFile commFile(QString("/proc/%1/comm").arg(pid));
  if (!commFile.open(QIODevice::ReadOnly | QIODevice::Text))
    return QString();

  return QString::fromUtf8(commFile.readAll()).trimmed();
}

QString WindowService::getWindowClassForWindow(unsigned long windowId) {
  Q_UNUSED(windowId)
  return QString();
}

QVariantList WindowService::getVisibleWindows() {
  return QVariantList();
}

} // namespace GameRecorder
