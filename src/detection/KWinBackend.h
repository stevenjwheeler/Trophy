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

  /** Install script, enable it, register the D-Bus object. */
  bool start();
  void stop();
  bool isActive() const { return m_active; }

  QList<WindowInfo> windows() const;

  /** Parse a script snapshot into window infos. Never throws. */
  static QList<WindowInfo> parseSnapshot(const QByteArray &json);

  // Script package state (used by the setup wizard + settings UI).
  static QString installedScriptDir();
  static bool isScriptInstalled();
  static bool isScriptEnabled();
  /** Install the bundled package if missing or outdated. */
  bool ensureScriptInstalled();
  /** Remove a previously installed package (used on explicit opt-out). */
  static bool removeInstalledScript();

signals:
  void windowsUpdated();

public slots:
  /** D-Bus entry point called by the KWin script. Also used by tests. */
  void ReportWindows(const QString &json);

private:
  // Locate the bundled script package: $TROPHY_KWIN_SCRIPT_DIR override,
  // installed datadir, then the CMake build-tree copy for dev runs.
  static QString findScriptSourceDir();
  bool installScript(bool *changed);
  bool ensureScriptEnabled(bool *changed);
  void reconfigureKWin();
  static void reconfigureKWinStatic();
  static bool runTool(const QString &tool, const QStringList &args,
                      QString *output = nullptr);

  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  bool m_active = false;
  bool m_dbusRegistered = false;
};

} // namespace TrophyBackend
