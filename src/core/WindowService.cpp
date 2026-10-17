#include "WindowService.h"
#include "detection/WaylandWindowMonitor.h"
#include "detection/WindowInfo.h"
#include "detection/GameIconSource.h"

#include <QDebug>
#include <QFile>
#include <QVariantMap>

namespace TrophyBackend {

WindowService::WindowService(QObject *parent) : QObject(parent) {}

void WindowService::setWindowMonitor(WaylandWindowMonitor *monitor) {
  m_monitor = monitor;
}

WindowService::~WindowService() {}

QString WindowService::getWindowTitle(unsigned long windowId) {
  return fetchWindowTitle(windowId);
}

bool WindowService::findWindow(quint64 id, WindowInfo *out) const {
  for (const WindowInfo &w : m_lastWindows) {
    if (w.id == id) {
      if (out)
        *out = w;
      return true;
    }
  }
  if (!m_monitor)
    return false;
  for (const WindowInfo &w : m_monitor->windows()) {
    if (w.id == id) {
      if (out)
        *out = w;
      return true;
    }
  }
  return false;
}

QString WindowService::fetchWindowTitle(unsigned long windowId) {
  WindowInfo w;
  if (findWindow(windowId, &w))
    return w.title;
  return QString();
}

unsigned long WindowService::getPidForWindow(unsigned long windowId) {
  WindowInfo w;
  if (findWindow(windowId, &w) && w.pid > 0)
    return (unsigned long)w.pid;
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
  WindowInfo w;
  if (findWindow(windowId, &w))
    return w.appId;
  return QString();
}

QVariantList WindowService::getVisibleWindows() {
  QVariantList out;
  if (!m_monitor)
    return out;
  // Cache the exact snapshot handed out so later id lookups resolve
  // against these rows even if the compositor pushes a new snapshot
  // before the caller looks the id up.
  m_lastWindows = m_monitor->windows();
  for (const WindowInfo &w : m_lastWindows) {
    QVariantMap m;
    m["id"] = QVariant::fromValue<quint64>(w.id);
    m["title"] = w.title;
    m["class"] = w.appId;
    m["iconSource"] = gameIconSource(w.appId);
    m["pid"] = QVariant::fromValue<qint64>(w.pid);
    out.append(m);
  }
  return out;
}

} // namespace TrophyBackend
