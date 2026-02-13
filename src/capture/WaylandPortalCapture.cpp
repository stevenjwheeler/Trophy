/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaylandPortalCapture.h"
#include <QDebug>
#include <QDBusConnection>
#include <QDBusInterface>

namespace GameRecorder {

WaylandPortalCapture::WaylandPortalCapture(QObject *parent)
    : IVideoCaptureSource(parent)
{
}

WaylandPortalCapture::~WaylandPortalCapture()
{
    stopCapture();
}

bool WaylandPortalCapture::initialize(const CaptureConfig &config)
{
    m_config = config;
    m_resolution = QSize(1920, 1080);  // Default, will be updated

    setupPortalConnection();

    qInfo() << "Wayland Portal capture initialized (stub)";
    return true;
}

void WaylandPortalCapture::setFrameCallback(FrameCallback callback)
{
    m_callback = std::move(callback);
}

bool WaylandPortalCapture::startCapture()
{
    if (m_capturing) {
        return false;
    }

    // In real implementation, we would start the PipeWire stream here
    m_capturing = true;
    emit captureStarted();

    qInfo() << "Wayland Portal capture started (stub)";
    return true;
}

void WaylandPortalCapture::stopCapture()
{
    if (!m_capturing) {
        return;
    }

    m_capturing = false;
    emit captureStopped();

    qInfo() << "Wayland Portal capture stopped";
}

bool WaylandPortalCapture::isCapturing() const
{
    return m_capturing;
}

CaptureCapabilities WaylandPortalCapture::capabilities() const
{
    CaptureCapabilities caps;
    caps.supportsWindowCapture = true;
    caps.supportsMonitorCapture = true;
    caps.supportsRegionCapture = false;
    caps.supportsCursorCapture = true;
    caps.supportsHardwareFrames = true;  // PipeWire can provide DMA-BUF
    caps.maxFps = 60;
    return caps;
}

QSize WaylandPortalCapture::nativeResolution() const
{
    return m_resolution;
}

void WaylandPortalCapture::requestScreenShare()
{
    // TODO: Implement XDG Desktop Portal screen share request
    // This would use D-Bus to communicate with the portal

    qInfo() << "Screen share request (stub) - would show portal dialog";

    // Simulate success for now
    emit screenShareGranted("pipewire-node-123");
}

void WaylandPortalCapture::setupPortalConnection()
{
    // TODO: Set up D-Bus connection to org.freedesktop.portal.ScreenCast
    qDebug() << "Setting up portal connection (stub)";
}

} // namespace GameRecorder
