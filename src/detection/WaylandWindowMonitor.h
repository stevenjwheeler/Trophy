/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <future>

namespace TrophyBackend {

class KWinBackend;
class GnomeBackend;
class HyprlandBackend;
class SwayBackend;

/**
 * @brief Monitors Wayland window/application activity.
 *
 * Wayland offers no cross-compositor window list, so each compositor gets
 * a backend: KWin (shipped script + D-Bus, implemented), then GNOME,
 * Hyprland and Sway. Backends push snapshots of WindowInfo structs;
 * this class resolves the focused app and notifies on change.
 *
 * WindowService shares this monitor to answer title/pid/class queries,
 * and GameDetectionService fuses focus + fullscreen with shim events.
 */
class WaylandWindowMonitor : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString focusedAppId READ focusedAppId NOTIFY focusedAppChanged)
  Q_PROPERTY(QString compositorName READ compositor NOTIFY windowsChanged)
  Q_PROPERTY(bool trackingActive READ isMonitoring NOTIFY windowsChanged)
  Q_PROPERTY(bool kwinScriptInstalled READ kwinScriptInstalled NOTIFY windowsChanged)
  Q_PROPERTY(bool kwinScriptEnabled READ kwinScriptEnabled NOTIFY windowsChanged)
  Q_PROPERTY(bool kwinActive READ kwinActive NOTIFY windowsChanged)

public:
  explicit WaylandWindowMonitor(QObject *parent = nullptr);
  ~WaylandWindowMonitor() override;

  void startMonitoring();
  void stopMonitoring();
  bool isMonitoring() const { return m_monitoring; }

  QString focusedAppId() const;
  QList<WindowInfo> windows() const;
  QString compositor() const { return m_compositor; }

  /** Canonical compositor key: kwin, gnome, hyprland, sway, or raw/unknown. */
  static QString detectCompositor();

  /** Window snapshot entry for a PID, or a default WindowInfo if unseen. */
  WindowInfo windowForPid(qint64 pid) const;

  // Compositor backend status for the setup wizard / settings UI.
  bool kwinScriptInstalled() const;
  bool kwinScriptEnabled() const;
  bool kwinActive() const;

  /** Stop tracking and remove the KWin script package we installed. */
  void uninstallBackend();

  /**
   * @brief Feed a compositor snapshot (D-Bus entry point for backends).
   * Public so tests can drive the monitor without a compositor.
   */
  void ingestSnapshot(const QList<WindowInfo> &windows);

signals:
  void focusedAppChanged(const QString &appId);
  void windowsChanged();

private slots:
  void checkFocusedApp(); // watchdog tick for polling backends
  void onBackendWindowsUpdated();

private:
  void setupCompositorTracking();

  QTimer *m_pollTimer = nullptr;
  QString m_focusedAppId;
  QList<WindowInfo> m_windows;
  mutable QMutex m_mutex;
  bool m_monitoring = false;
  QString m_compositor;
  KWinBackend *m_kwin = nullptr;
  GnomeBackend *m_gnome = nullptr;
  HyprlandBackend *m_hyprland = nullptr;
  SwayBackend *m_sway = nullptr;
  std::future<void> m_pollFuture;
};

} // namespace TrophyBackend
