/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CaptureFactory.h"
#include <QDebug>
#include <QProcessEnvironment>
#include <QDBusInterface>
#include <QDBusConnection>

#include "IVideoCaptureSource.h"

#ifdef HAVE_PIPEWIRE
#include "WaylandPortalCapture.h"
#endif

namespace TrophyBackend {

CaptureFactory::DisplayServer CaptureFactory::detectDisplayServer() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

  // Check XDG_SESSION_TYPE first (most reliable)
  QString sessionType = env.value("XDG_SESSION_TYPE", "").toLower();
  if (sessionType == "wayland") {
    return DisplayServer::Wayland;
  }
  if (sessionType == "x11") {
    return DisplayServer::X11;
  }

  // Check WAYLAND_DISPLAY
  if (!env.value("WAYLAND_DISPLAY", "").isEmpty()) {
    return DisplayServer::Wayland;
  }

  // Check DISPLAY for X11
  if (!env.value("DISPLAY", "").isEmpty()) {
    return DisplayServer::X11;
  }

  qWarning() << "Could not detect display server";
  return DisplayServer::Unknown;
}

std::unique_ptr<IVideoCaptureSource> CaptureFactory::createBestSource() {
  DisplayServer server = detectDisplayServer();
  qDebug() << "Detected display server:" << displayServerName(server);

  return createSource(server);
}


std::unique_ptr<IVideoCaptureSource>
CaptureFactory::createSource(DisplayServer server) {
  switch (server) {
  case DisplayServer::X11:
    qWarning() << "X11 capture is no longer supported";
    return nullptr;

  case DisplayServer::Wayland:
#ifdef HAVE_PIPEWIRE
    qDebug() << "Creating Wayland Portal capture source";
    return std::make_unique<WaylandPortalCapture>();
#else
    qWarning() << "Wayland/PipeWire support not compiled in";
    return nullptr;
#endif

  case DisplayServer::Unknown:
  default:
    qCritical() << "Cannot create capture source: unknown display server";
    return nullptr;
  }
}

QString CaptureFactory::displayServerName(DisplayServer server) {
  switch (server) {
  case DisplayServer::X11:
    return "X11";
  case DisplayServer::Wayland:
    return "Wayland";
  case DisplayServer::Unknown:
  default:
    return "Unknown";
  }
}

bool CaptureFactory::isPipeWireAvailable() {
#ifndef HAVE_PIPEWIRE
  return false;
#else
  // On Wayland, we need the ScreenCast portal.
  // We can check if the interface exists on D-Bus.
  QDBusInterface portal("org.freedesktop.portal.Desktop",
                         "/org/freedesktop/portal/desktop",
                         "org.freedesktop.portal.ScreenCast",
                         QDBusConnection::sessionBus());
  return portal.isValid();
#endif
}

} // namespace TrophyBackend
