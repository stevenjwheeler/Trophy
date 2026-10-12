/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaylandWindowMonitor.h"
#include "GnomeBackend.h"
#include "HyprlandBackend.h"
#include "KWinBackend.h"
#include "SwayBackend.h"

#include <QDebug>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <future>

namespace TrophyBackend {

WaylandWindowMonitor::WaylandWindowMonitor(QObject *parent) : QObject(parent) {
  // Detect eagerly so UI (wizard, settings) can name the compositor even
  // when tracking is opted out and startMonitoring() never runs.
  m_compositor = detectCompositor();
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(1000);
  connect(m_pollTimer, &QTimer::timeout, this,
          &WaylandWindowMonitor::checkFocusedApp);
}

WaylandWindowMonitor::~WaylandWindowMonitor() { stopMonitoring(); }

void WaylandWindowMonitor::startMonitoring() {
  if (m_monitoring) {
    return;
  }

  m_compositor = detectCompositor();
  qDebug() << "Wayland compositor detected:" << m_compositor;

  setupCompositorTracking();

  m_monitoring = true;
  m_pollTimer->start();

  qDebug() << "Wayland window monitoring started (" << m_compositor << ")";
  // The KWin installed/enabled properties notify on windowsChanged, which
  // only fires on snapshots — re-read them now so status labels shown
  // right after a toggle reflect the install that just ran, not the
  // pre-toggle state.
  emit windowsChanged();
}

void WaylandWindowMonitor::stopMonitoring() {
  if (!m_monitoring && !m_kwin && !m_gnome && !m_hyprland && !m_sway) {
    return;
  }

  m_monitoring = false;
  m_pollTimer->stop();
  // Polling runs off the GUI thread because the compositor CLI/D-Bus calls
  // have bounded but potentially multi-second timeouts. Join before deleting
  // backend objects that the worker may still be using.
  if (m_pollFuture.valid())
    m_pollFuture.wait();
  if (m_kwin) {
    m_kwin->stop();
    m_kwin->deleteLater();
    m_kwin = nullptr;
  }
  if (m_gnome) {
    m_gnome->stop();
    m_gnome->deleteLater();
    m_gnome = nullptr;
  }
  if (m_hyprland) {
    m_hyprland->stop();
    m_hyprland->deleteLater();
    m_hyprland = nullptr;
  }
  if (m_sway) {
    m_sway->stop();
    m_sway->deleteLater();
    m_sway = nullptr;
  }

  qInfo() << "Wayland window monitoring stopped";
}

QString WaylandWindowMonitor::focusedAppId() const {
  QMutexLocker lg(&m_mutex);
  return m_focusedAppId;
}

QList<WindowInfo> WaylandWindowMonitor::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

WindowInfo WaylandWindowMonitor::windowForPid(qint64 pid) const {
  QMutexLocker lg(&m_mutex);
  for (const WindowInfo &w : m_windows) {
    if (w.pid == pid)
      return w;
  }
  return WindowInfo{};
}

bool WaylandWindowMonitor::kwinScriptInstalled() const {
  return KWinBackend::isScriptInstalled();
}

bool WaylandWindowMonitor::kwinScriptEnabled() const {
  return KWinBackend::isScriptEnabled();
}

bool WaylandWindowMonitor::kwinActive() const {
  return m_kwin && m_kwin->isActive();
}

void WaylandWindowMonitor::uninstallBackend() {
  stopMonitoring();
  KWinBackend::removeInstalledScript();
  // Same staleness reason as in startMonitoring: refresh status bindings.
  emit windowsChanged();
}

void WaylandWindowMonitor::ingestSnapshot(const QList<WindowInfo> &windows) {
  QString newFocus;
  for (const WindowInfo &w : windows) {
    if (w.focused && !w.appId.isEmpty()) {
      newFocus = w.appId;
      break;
    }
  }
  // Fall back to the focused window's title when the class is hidden.
  if (newFocus.isEmpty()) {
    for (const WindowInfo &w : windows) {
      if (w.focused && !w.title.isEmpty()) {
        newFocus = w.title;
        break;
      }
    }
  }

  bool focusChanged = false;
  {
    QMutexLocker lg(&m_mutex);
    m_windows = windows;
    if (newFocus != m_focusedAppId) {
      m_focusedAppId = newFocus;
      focusChanged = true;
    }
  }
  emit windowsChanged();
  if (focusChanged)
    emit focusedAppChanged(newFocus);
}

void WaylandWindowMonitor::checkFocusedApp() {
  // Watchdog tick. The KWin backend is event-driven (the script pushes on
  // window changes); CLI polling backends refresh here.
  if (!m_monitoring)
    return;
  if (m_pollFuture.valid()) {
    if (m_pollFuture.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    try {
      m_pollFuture.get();
    } catch (const std::exception &e) {
      qWarning() << "Compositor poll failed:" << e.what();
    }
  }
  try {
    m_pollFuture = std::async(std::launch::async, [this]() {
      if (m_gnome)
        m_gnome->refresh();
      if (m_hyprland)
        m_hyprland->refresh();
      if (m_sway)
        m_sway->refresh();
    });
  } catch (const std::exception &e) {
    qWarning() << "Unable to start compositor poll:" << e.what();
  }
}

void WaylandWindowMonitor::onBackendWindowsUpdated() {
  QObject *src = sender();
  if (m_kwin && src == m_kwin)
    ingestSnapshot(m_kwin->windows());
  else if (m_gnome && src == m_gnome)
    ingestSnapshot(m_gnome->windows());
  else if (m_hyprland && src == m_hyprland)
    ingestSnapshot(m_hyprland->windows());
  else if (m_sway && src == m_sway)
    ingestSnapshot(m_sway->windows());
}

void WaylandWindowMonitor::setupCompositorTracking() {
  if (m_compositor.contains("kwin", Qt::CaseInsensitive)) {
    if (!m_kwin) {
      m_kwin = new KWinBackend(this);
      connect(m_kwin, &KWinBackend::windowsUpdated, this,
              &WaylandWindowMonitor::onBackendWindowsUpdated);
    }
    if (!m_kwin->start())
      qWarning() << "KWin backend failed to start; window data unavailable";
  } else if (m_compositor.contains("gnome", Qt::CaseInsensitive) ||
             m_compositor.contains("mutter", Qt::CaseInsensitive)) {
    if (!m_gnome) {
      m_gnome = new GnomeBackend(this);
      connect(m_gnome, &GnomeBackend::windowsUpdated, this,
              &WaylandWindowMonitor::onBackendWindowsUpdated);
    }
    if (!m_gnome->start())
      qWarning() << "GNOME backend failed to start; window data unavailable";
  } else if (m_compositor.contains("hyprland", Qt::CaseInsensitive)) {
    if (!m_hyprland) {
      m_hyprland = new HyprlandBackend(this);
      connect(m_hyprland, &HyprlandBackend::windowsUpdated, this,
              &WaylandWindowMonitor::onBackendWindowsUpdated);
    }
    if (!m_hyprland->start())
      qWarning() << "Hyprland backend failed to start; window data unavailable";
  } else if (m_compositor.contains("sway", Qt::CaseInsensitive)) {
    if (!m_sway) {
      m_sway = new SwayBackend(this);
      connect(m_sway, &SwayBackend::windowsUpdated, this,
              &WaylandWindowMonitor::onBackendWindowsUpdated);
    }
    if (!m_sway->start())
      qWarning() << "Sway backend failed to start; window data unavailable";
  } else {
    qWarning() << "Unknown compositor, window tracking unavailable:"
               << m_compositor;
  }
}

QString WaylandWindowMonitor::detectCompositor() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

  // Canonicalize to the keys setupCompositorTracking understands. Notably
  // KDE sessions report XDG_CURRENT_DESKTOP=KDE (not KWin).
  auto canonical = [](const QString &raw) -> QString {
    const QString v = raw.toLower();
    if (v.isEmpty())
      return QString();
    if (v.contains("kwin") || v.contains("kde") || v.contains("plasma"))
      return "kwin";
    if (v.contains("gnome") || v.contains("mutter"))
      return "gnome";
    if (v.contains("hyprland"))
      return "hyprland";
    if (v.contains("sway"))
      return "sway";
    return v;
  };

  const QString desktop = canonical(env.value("XDG_CURRENT_DESKTOP", ""));
  if (!desktop.isEmpty())
    return desktop;

  const QString session = canonical(env.value("DESKTOP_SESSION", ""));
  if (!session.isEmpty())
    return session;

  // Check for specific compositor env vars
  if (!env.value("HYPRLAND_INSTANCE_SIGNATURE", "").isEmpty()) {
    return "hyprland";
  }
  if (!env.value("SWAYSOCK", "").isEmpty()) {
    return "sway";
  }
  if (!env.value("KDE_SESSION_VERSION", "").isEmpty()) {
    return "kwin";
  }

  return "unknown";
}

} // namespace TrophyBackend
