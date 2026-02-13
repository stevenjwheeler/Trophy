/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

namespace GameRecorder {

/**
 * @brief Monitors X11 window focus and properties
 *
 * Watches for window focus changes and provides information
 * about the currently focused window.
 */
class X11WindowMonitor : public QObject {
  Q_OBJECT

public:
  struct WindowInfo {
    unsigned long windowId;
    QString windowClass;
    QString title;
    qint64 pid;
    int width;
    int height;
  };

  explicit X11WindowMonitor(QObject *parent = nullptr);
  ~X11WindowMonitor() override;

  void startMonitoring();
  void stopMonitoring();

  WindowInfo getActiveWindowInfo() const;
  WindowInfo getWindowInfo(unsigned long windowId) const;

signals:
  void activeWindowChanged(unsigned long windowId);
  void windowCreated(unsigned long windowId);
  void windowDestroyed(unsigned long windowId);

private:
  bool m_monitoring = false;
};

} // namespace GameRecorder
