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
#include <memory>
#include <vector>

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
  Q_PROPERTY(bool trackingReady READ trackingReady NOTIFY windowsChanged)
  Q_PROPERTY(bool trackingChecking READ trackingChecking NOTIFY windowsChanged)

public:
  explicit WaylandWindowMonitor(QObject *parent = nullptr);
  ~WaylandWindowMonitor() override;

  /** Missing/disabled helpers require an explicit Install/Repair action. */
  void startMonitoring(bool repairBackend = false);
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
  bool trackingReady() const;
  bool trackingChecking() const { return m_healthTimer.isActive(); }
  Q_INVOKABLE void repairTracking();

  /** Stop tracking and remove the KWin script package we installed. */
  void uninstallBackend();

  /**
   * @brief Feed a compositor snapshot (D-Bus entry point for backends).
   * Public so tests can drive the monitor without a compositor.
   *
   * Entries whose compositor id is 0 (the KWin script reports
   * client.windowId, which is 0 for every Wayland window) are remapped so
   * every stored row has a distinct id and WindowService lookups resolve
   * the correct row instead of always matching the first one. Remap
   * chain: real nonzero id kept; zero id + stable string key (KWin
   * internalId) -> deterministic key-derived id (tracks across snapshots
   * and restarts); zero id + no key -> kSyntheticIdBase + position (stable
   * only while the backend's order holds). Real nonzero compositor ids
   * (GNOME/Hyprland/Sway) pass through untouched. No stored row keeps id
   * 0 afterwards. Consumers must still resolve ids promptly (the
   * WindowService snapshot cache covers the list-then-lookup gap).
   */
  void ingestSnapshot(const QList<WindowInfo> &windows);

  /**
   * @brief Base for synthetic window ids assigned at ingest time.
   *
   * 0x80000000 is far above the small ids GNOME (stable sequence) and
   * Sway (container ids) hand out, and far below Hyprland's 48-bit
   * window addresses, so remapped KWin rows cannot collide with real
   * compositor ids in practice. No stored row keeps id 0 afterwards.
   */
  static constexpr quint64 kSyntheticIdBase = 0x80000000ULL;

signals:
  void focusedAppChanged(const QString &appId);
  void windowsChanged();

private slots:
  void checkFocusedApp(); // watchdog tick for polling backends
  void onBackendWindowsUpdated();

private:
  friend class WindowMonitorWorkerTest;
  void setupCompositorTracking(bool repairBackend);
  void collectRetired();
  struct Retired {
    std::shared_ptr<QObject> backend;
    std::future<void> poll;
  };
  std::vector<Retired> m_retired;
  std::vector<std::shared_future<void>> m_removals;
  quint64 m_generation = 0;
  std::function<void()> m_pollOverride; // private deterministic worker seam

  QTimer *m_pollTimer = nullptr;
  QTimer m_healthTimer;
  QString m_focusedAppId;
  QList<WindowInfo> m_windows;
  mutable QMutex m_mutex;
  bool m_monitoring = false;
  bool m_lastTrackingReady = false;
  QString m_compositor;
  std::shared_ptr<KWinBackend> m_kwin;
  std::shared_ptr<GnomeBackend> m_gnome;
  std::shared_ptr<HyprlandBackend> m_hyprland;
  std::shared_ptr<SwayBackend> m_sway;
  std::future<void> m_pollFuture;
};

} // namespace TrophyBackend
