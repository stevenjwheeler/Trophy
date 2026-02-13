/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaylandWindowMonitor.h"
#include <QDebug>
#include <QProcessEnvironment>

namespace GameRecorder {

WaylandWindowMonitor::WaylandWindowMonitor(QObject *parent)
    : QObject(parent)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1000);
    connect(m_pollTimer, &QTimer::timeout, this, &WaylandWindowMonitor::checkFocusedApp);
}

WaylandWindowMonitor::~WaylandWindowMonitor()
{
    stopMonitoring();
}

void WaylandWindowMonitor::startMonitoring()
{
    if (m_monitoring) {
        return;
    }

    QString compositor = detectCompositor();
    qInfo() << "Wayland compositor detected:" << compositor;

    setupCompositorTracking();

    m_monitoring = true;
    m_pollTimer->start();

    qInfo() << "Wayland window monitoring started (stub)";
}

void WaylandWindowMonitor::stopMonitoring()
{
    if (!m_monitoring) {
        return;
    }

    m_monitoring = false;
    m_pollTimer->stop();

    qInfo() << "Wayland window monitoring stopped";
}

QString WaylandWindowMonitor::focusedAppId() const
{
    return m_focusedAppId;
}

void WaylandWindowMonitor::checkFocusedApp()
{
    // TODO: Query compositor for focused window/app
    // This is compositor-specific:
    // - KDE: KWin scripting or D-Bus
    // - GNOME: gnome-shell D-Bus extension
    // - Hyprland: IPC socket
    // - Sway: IPC

    // For now, just a stub
}

void WaylandWindowMonitor::setupCompositorTracking()
{
    QString compositor = detectCompositor();

    if (compositor.contains("kwin", Qt::CaseInsensitive)) {
        // TODO: Set up KWin D-Bus connection
        qDebug() << "Would set up KWin tracking";
    } else if (compositor.contains("gnome", Qt::CaseInsensitive) ||
               compositor.contains("mutter", Qt::CaseInsensitive)) {
        // TODO: Set up GNOME Shell extension communication
        qDebug() << "Would set up GNOME tracking";
    } else if (compositor.contains("hyprland", Qt::CaseInsensitive)) {
        // TODO: Set up Hyprland IPC
        qDebug() << "Would set up Hyprland tracking";
    } else if (compositor.contains("sway", Qt::CaseInsensitive)) {
        // TODO: Set up Sway IPC
        qDebug() << "Would set up Sway tracking";
    } else {
        qWarning() << "Unknown compositor, window tracking may be limited";
    }
}

QString WaylandWindowMonitor::detectCompositor() const
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    // Check XDG_CURRENT_DESKTOP
    QString desktop = env.value("XDG_CURRENT_DESKTOP", "").toLower();
    if (!desktop.isEmpty()) {
        return desktop;
    }

    // Check DESKTOP_SESSION
    QString session = env.value("DESKTOP_SESSION", "").toLower();
    if (!session.isEmpty()) {
        return session;
    }

    // Check for specific compositor env vars
    if (!env.value("HYPRLAND_INSTANCE_SIGNATURE", "").isEmpty()) {
        return "hyprland";
    }
    if (!env.value("SWAYSOCK", "").isEmpty()) {
        return "sway";
    }

    return "unknown";
}

} // namespace GameRecorder
