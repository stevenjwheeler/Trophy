/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "CaptureFactory.h"
#include <QDebug>
#include <QProcessEnvironment>

#include "IVideoCaptureSource.h"

#ifdef HAVE_X11
#include "X11WindowCapture.h"
#endif

#ifdef HAVE_PIPEWIRE
#include "WaylandPortalCapture.h"
#endif

namespace GameRecorder {

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
  qInfo() << "Detected display server:" << displayServerName(server);

  return createSource(server);
}

std::unique_ptr<IVideoCaptureSource> CaptureFactory::createX11Source() {
  return createSource(DisplayServer::X11);
}

std::unique_ptr<IVideoCaptureSource>
CaptureFactory::createSource(DisplayServer server) {
  switch (server) {
  case DisplayServer::X11:
#ifdef HAVE_X11
    qInfo() << "Creating X11 capture source";
    return std::make_unique<X11WindowCapture>();
#else
    qWarning() << "X11 support not compiled in";
    return nullptr;
#endif

  case DisplayServer::Wayland:
#ifdef HAVE_PIPEWIRE
    qInfo() << "Creating Wayland Portal capture source";
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

} // namespace GameRecorder
