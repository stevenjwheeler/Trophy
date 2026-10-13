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
#include <atomic>
#include <future>
#include <functional>
#include <QTimer>

// Forward declaration for the test seam below (fixture lives in the
// global namespace, outside TrophyBackend).
class KWinScriptInstallTest;

namespace TrophyBackend {

/**
 * @brief KWin window-tracking backend for WaylandWindowMonitor.
 *
 * Installs and enables the bundled `trophy-monitor` KWin script package
 * (resources/kwin/trophy-monitor) via kpackagetool6 — the official route,
 * so the package shows up in System Settings > KWin Scripts where the
 * user can inspect, disable or remove it. The script pushes JSON window
 * snapshots to the org.trophy.Monitor D-Bus object implemented here.
 * Read-only: the script never moves, resizes or closes windows.
 *
 * Snapshot format: [{id, appId, title, pid, fullscreen, focused}, ...]
 *
 * The static parseSnapshot() is pure and unit-tested with canned JSON so
 * the backend works without a running compositor in tests/CI.
 */
class KWinBackend : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.trophy.Monitor")

public:
  static constexpr const char *kServiceName = "org.trophy.Monitor";
  static constexpr const char *kObjectPath = "/Monitor";
  static constexpr const char *kScriptId = "trophy-monitor";

  explicit KWinBackend(QObject *parent = nullptr);
  ~KWinBackend() override;

  /** Install script, enable it, register the D-Bus object.
   *
   * D-Bus registration happens synchronously (fast); the blocking
   * install/enable tool sequence runs on a worker and its results are
   * applied back on the owner thread (windowsUpdated so
   * WaylandWindowMonitor refreshes its status labels). Returns true once
   * the D-Bus object is registered, not once the script is installed. */
  bool start();
  void stop();
  bool isActive() const { return m_active; }

  QList<WindowInfo> windows() const;

  /** Parse a script snapshot into window infos. Never throws. */
  static QList<WindowInfo> parseSnapshot(const QByteArray &json);

  // Script package state (used by the setup wizard + settings UI).
  static QString installedScriptDir();
  static bool isScriptInstalled();
  /** Cached enable state from the last setup sequence (default false while
   * unknown). Served from cache so status-label reads never spawn tools on
   * the GUI thread; refreshed by start()'s worker and removeInstalledScript.
   * Use queryScriptEnabledLive() when a synchronous answer is required. */
  static bool isScriptEnabled();
  /** Bundled package under an install prefix (AppImage included). */
  static QString findScriptSourceDirForAppDir(const QString &appDir);
  /** Install the bundled package if missing or outdated.
   * Stays synchronous: KWinScriptInstallTest.InstallRemoveRoundTrip asserts
   * the on-disk effect (and a no-op second run) on return. Production
   * startup never calls this — start()'s worker uses installScript(). */
  bool ensureScriptInstalled();
  /** Remove a previously installed package (used on explicit opt-out).
   * Stays synchronous: KWinScriptInstallTest.InstallRemoveRoundTrip and
   * RemoveWhenAbsentSucceeds assert the removal result and the on-disk
   * state on return. Production opt-out uses the owned asynchronous wrapper,
   * ordered after any earlier setup side effects. */
  static bool removeInstalledScript();
  /** Worker-capable synchronous removal core behind removeInstalledScript().
   * Touches only the filesystem, tool spawns and s_enabledCache — never
   * member state — so it is safe to run on a worker thread. */
  static bool removeInstalledScriptSync();
  static std::shared_future<void> removeInstalledScriptAsync();
  bool hasPendingWork() const;

signals:
  void windowsUpdated();

public slots:
  /** D-Bus entry point called by the KWin script. Also used by tests. */
  void ReportWindows(const QString &json);

private:
  // Locate the bundled script package: $TROPHY_KWIN_SCRIPT_DIR override,
  // our own install prefix (AppImage included), installed datadir,
  // then the CMake build-tree copy for dev runs.
  static QString findScriptSourceDir();
  // Install/enable helpers below are static so start()'s worker can run
  // them without touching member state (safe under a racing stop()).
  static bool installScript(bool *changed);
  static bool ensureScriptEnabled(bool *changed);
  /** Synchronous kreadconfig query. Worker/caller context only — never from
   * a status-label read (see isScriptEnabled). */
  static bool queryScriptEnabledLive();
  static void reconfigureKWinStatic();
  static bool runTool(const QString &tool, const QStringList &args,
                      QString *output = nullptr);

  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  bool m_active = false;
  bool m_dbusRegistered = false;
  // Test seam: the install-script fixture resets the static enable cache
  // so a future test running the setup worker can't flip
  // EnabledFalseWithoutKwinTools order-dependently. (Global scope: the
  // test fixture lives outside namespace TrophyBackend.)
  friend class ::KWinScriptInstallTest;
  friend class KWinWorkerTest;
  void queueSetup();
  static std::shared_future<void> enqueueOperation(std::function<void()> operation);
  static std::function<bool(const QString &, const QStringList &, QString *)> s_toolOverride;
  static std::function<void()> s_restartOverride;
  std::shared_future<void> m_setupFuture;
  QTimer *m_setupTimer = nullptr;
  // Owner-thread delivery generation. Side effects are separately ordered
  // by enqueueOperation, including setup followed by explicit removal.
  quint64 m_setupGeneration = 0;
  // Cached enable state (-1 unknown, 0 disabled, 1 enabled), written by the
  // setup worker on delivery and by removeInstalledScriptSync(); read lock-free by
  // isScriptEnabled().
  static std::atomic<int> s_enabledCache;
};

} // namespace TrophyBackend
