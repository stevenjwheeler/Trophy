/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "X11WindowMonitor.h"
#include <QDebug>

namespace GameRecorder {

X11WindowMonitor::X11WindowMonitor(QObject *parent) : QObject(parent) {}

X11WindowMonitor::~X11WindowMonitor() { stopMonitoring(); }

void X11WindowMonitor::startMonitoring() {
  if (m_monitoring) {
    return;
  }

  // TODO: Connect to X11 display and subscribe to events
  m_monitoring = true;
  qInfo() << "X11 window monitoring started (stub)";
}

void X11WindowMonitor::stopMonitoring() {
  if (!m_monitoring) {
    return;
  }

  m_monitoring = false;
  qInfo() << "X11 window monitoring stopped";
}

X11WindowMonitor::WindowInfo X11WindowMonitor::getActiveWindowInfo() const {
  // TODO: Get actual active window using _NET_ACTIVE_WINDOW
  WindowInfo info;
  info.windowId = 0;
  info.windowClass = "";
  info.title = "";
  info.pid = 0;
  info.width = 0;
  info.height = 0;
  return info;
}

X11WindowMonitor::WindowInfo
X11WindowMonitor::getWindowInfo(unsigned long windowId) const {
  Q_UNUSED(windowId)

  // TODO: Query window properties using X11
  WindowInfo info;
  info.windowId = windowId;
  return info;
}

} // namespace GameRecorder
