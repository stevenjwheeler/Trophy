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

namespace TrophyBackend {

/**
 * @brief Hyprland window-tracking backend for WaylandWindowMonitor.
 *
 * Polls `hyprctl clients -j` (+ `activewindow -j` for focus) on the
 * monitor's 1s tick. No installation, no socket protocol, nothing
 * written anywhere — read-only CLI queries. Window ids are client
 * addresses; fullscreen covers Hyprland modes 2/3 (mode 1 is maximized).
 *
 * The static parseClients() is pure and unit-tested with canned JSON.
 */
class HyprlandBackend : public QObject {
  Q_OBJECT

public:
  explicit HyprlandBackend(QObject *parent = nullptr);
  ~HyprlandBackend() override = default;

  bool start();
  void stop() { m_active.store(false); }
  bool isActive() const { return m_active.load(); }

  /** One poll round-trip; called by the monitor's 1s watchdog tick. */
  void refresh();

  QList<WindowInfo> windows() const;

  /**
   * @brief Parse `hyprctl clients -j` output.
   * @param activeAddress address from `activewindow -j` (empty = unknown,
   *        falls back to focusHistoryID == 0).
   * @param monitorsJson `hyprctl monitors -j` output for output sizes
   *        (empty = unknown, coverage rule disabled).
   */
  static QList<WindowInfo> parseClients(const QByteArray &json,
                                        const QString &activeAddress,
                                        const QByteArray &monitorsJson = {});

  /** Stable content signature used to skip unchanged snapshots. */
  static QString signatureFor(const QList<WindowInfo> &windows);

signals:
  void windowsUpdated();

private:
  static QString runHyprctl(const QStringList &args);

  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature; // content hash of the last emitted snapshot
  std::atomic_bool m_active{false};
};

} // namespace TrophyBackend
