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
 * @brief Sway window-tracking backend for WaylandWindowMonitor.
 *
 * Runs `swaymsg -t get_tree` on the monitor's 1s tick and walks the
 * container tree for windows (tiled `con` and floating nodes). No
 * installation, no event subscription, nothing written anywhere.
 * Native Wayland app_id preferred, XWayland window class as fallback;
 * fullscreen_mode != 0 counts as fullscreen.
 *
 * The static parseTree() is pure and unit-tested with a canned tree.
 */
class SwayBackend : public QObject {
  Q_OBJECT

public:
  explicit SwayBackend(QObject *parent = nullptr);
  ~SwayBackend() override = default;

  bool start();
  void stop() { m_active = false; }
  bool isActive() const { return m_active; }

  /** One poll round-trip; called by the monitor's 1s watchdog tick. */
  void refresh();

  QList<WindowInfo> windows() const;

  /** Parse `swaymsg -t get_tree` output into window infos. */
  static QList<WindowInfo> parseTree(const QByteArray &json);

signals:
  void windowsUpdated();

private:
  static void collectWindows(const QJsonObject &node, QList<WindowInfo> &out,
                             int outputWidth, int outputHeight);

  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  bool m_active = false;
};

} // namespace TrophyBackend
