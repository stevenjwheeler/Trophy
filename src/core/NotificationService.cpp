/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NotificationService.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QFileInfo>

namespace GameRecorder {

NotificationService::NotificationService(QObject *parent) : QObject(parent) {
  QDBusConnection::sessionBus().connect(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "ActionInvoked", this,
      SLOT(onActionInvoked(uint, QString)));

  QDBusConnection::sessionBus().connect(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "NotificationClosed", this,
      SLOT(onNotificationClosed(uint, uint)));
}

NotificationService::~NotificationService() = default;

void NotificationService::setEnabled(bool enabled) {
  if (m_enabled != enabled) {
    m_enabled = enabled;
    emit enabledChanged();
  }
}

void NotificationService::showNotification(const QString &title,
                                           const QString &body,
                                           const QString &icon, int timeout,
                                           int urgency) {
  if (!m_enabled) {
    return;
  }

  QDBusInterface notify(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", QDBusConnection::sessionBus());

  if (!notify.isValid()) {
    qWarning() << "D-Bus notification interface not available";
    return;
  }

  QString appIcon = icon.isEmpty() ? "trophy" : icon;

  QVariantMap hints;
  hints["urgency"] = QVariant::fromValue<uchar>(static_cast<uchar>(urgency));
  hints["resident"] = true; // Stay in the notification center

  QStringList actions;
  if (urgency > 1) { // Only add buttons for high-priority authorization
    actions << "authorize" << "Authorize Capture";
  }

  QDBusReply<uint> reply =
      notify.call("Notify",
                  "Trophy",             // app_name (keep it short)
                  m_lastNotificationId, // replaces_id
                  appIcon,              // app_icon
                  title,                // summary
                  body,                 // body
                  actions,              // actions
                  hints,                // hints
                  timeout               // timeout (ms)
      );

  if (reply.isValid()) {
    m_lastNotificationId = reply.value();
  } else {
    qWarning() << "Failed to send notification:" << reply.error().message();
  }
}

void NotificationService::onActionInvoked(uint id, const QString &action) {
  if (id == m_lastNotificationId || m_lastNotificationId == 0) {
    emit notificationClicked(action);
  }
}

void NotificationService::onNotificationClosed(uint id, uint reason) {
  if (id == m_lastNotificationId) {
    m_lastNotificationId = 0;
  }
}

void NotificationService::showRecordingStarted(const QString &gameName) {
  QString title = "Recording Started";
  QString body = gameName.isEmpty() ? "Recording has started"
                                    : QString("Recording %1").arg(gameName);

  showNotification(title, body, "media-record");
}

void NotificationService::showRecordingStopped(const QString &filePath) {
  QString fileName = QFileInfo(filePath).fileName();
  showNotification("Recording Saved", QString("Saved to %1").arg(fileName),
                   "media-playback-stop");
}

void NotificationService::showError(const QString &message) {
  showNotification("Recording Error", message, "dialog-error");
}

} // namespace GameRecorder
