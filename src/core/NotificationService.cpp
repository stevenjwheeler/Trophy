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
#include <QDateTime>
#include <QTimer>

namespace TrophyBackend {

NotificationService::NotificationService(QObject *parent) : QObject(parent) {
  m_dismissTimer = new QTimer(this);
  m_dismissTimer->setSingleShot(true);
  connect(m_dismissTimer, &QTimer::timeout, this, [this]() {
    closeNotificationById(m_dismissId);
  });
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

bool NotificationService::isRepeatSuppressed(const QString &title,
                                              const QString &body,
                                              const QString &lastTitle,
                                              const QString &lastBody,
                                              qint64 elapsedMs,
                                              qint64 cooldownMs) {
  return title == lastTitle && body == lastBody && elapsedMs >= 0 &&
         elapsedMs < cooldownMs;
}

QVariantMap
NotificationService::notificationHints(int urgency, bool authorizeAction) {
  QVariantMap hints;
  hints["urgency"] = QVariant::fromValue<uchar>(static_cast<uchar>(urgency));
  // Resident (stay in the notification center) is for the authorize prompt
  // only. Transient notices must expire fully — with resident set, Plasma
  // keeps every clip/recording notice until manually dismissed.
  if (authorizeAction)
    hints["resident"] = true;
  return hints;
}

void NotificationService::closeLastNotification() {
  if (m_lastNotificationId == 0)
    return;
  closeNotificationById(m_lastNotificationId);
  m_lastNotificationId = 0;
}

void NotificationService::closeNotificationById(uint id) {
  QDBusInterface notify(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", QDBusConnection::sessionBus());
  if (notify.isValid())
    // Fire-and-forget: must never block the GUI thread on plasmashell.
    notify.asyncCall("CloseNotification", id);
  // Deliberately leaves m_lastNotificationId alone: click-matching for a
  // newer prompt (e.g. authorize) must keep working after an older
  // transient notice is withdrawn.
}
void NotificationService::showNotification(const QString &title,
                                           const QString &body,
                                           const QString &icon, int timeout,
                                           int urgency,
                                            bool authorizeAction) {
  if (!m_enabled) {
    return;
  }

  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  const qint64 elapsed =
      m_lastSentMs < 0 ? -1 : (nowMs - m_lastSentMs);
  if (isRepeatSuppressed(title, body, m_lastTitle, m_lastBody, elapsed)) {
    qDebug() << "Suppressing duplicate notification within cooldown:" << title;
    return;
  }
  m_lastTitle = title;
  m_lastBody = body;
  m_lastSentMs = nowMs;

  QDBusInterface notify(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", QDBusConnection::sessionBus());

  if (!notify.isValid()) {
    qWarning() << "D-Bus notification interface not available";
    return;
  }

  QString appIcon = icon.isEmpty() ? "trophy" : icon;

  const QVariantMap hints = notificationHints(urgency, authorizeAction);

  QStringList actions;
  // The authorize button belongs only to the DVR authorize prompt — it
  // used to ride along with any urgency>1 notification, which would have
  // put an "Authorize Capture" button on critical recording/clip notices.
  if (authorizeAction) {
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
    // Self-dismiss transient notices: timeout 0 means "stay until acted
    // on" (the authorize prompt); anything else restarts the single
    // countdown on the latest id, so a replacing notice ("Saved") takes
    // over from the one it replaced ("Saving…") instead of racing it.
    m_dismissTimer->stop();
    if (timeout > 0) {
      m_dismissId = reply.value();
      m_dismissTimer->start(timeout);
    }
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
  Q_UNUSED(reason);
  // Body-clicking a notification closes it without ActionInvoked, so the
  // authorize channel would never fire for body clicks. Treat closing the
  // authorize prompt as acknowledgement. Other notifications are ignored
  // here (Application no-ops without pending authorize anyway, but keep
  // this narrow so a stray close can't arm anything unexpected).
  const bool isAuthorize =
      m_lastTitle.startsWith(QStringLiteral("New Game: "));
  if (id == m_lastNotificationId && m_lastNotificationId != 0) {
    m_lastNotificationId = 0;
    if (isAuthorize)
      emit notificationClicked(QString());
  } else if (id == m_lastNotificationId) {
    m_lastNotificationId = 0;
  }
}

void NotificationService::showRecordingStarted(const QString &gameName) {
  QString title = "Recording Started";
  QString body = gameName.isEmpty() ? "Recording has started"
                                    : QString("Recording %1").arg(gameName);

  // Critical so the notice surfaces over a fullscreen game/screenshare.
  // 3s: long enough to read, short enough to be gone (self-withdrawn).
  showNotification(title, body, "trophy", 3000, 2);
}

void NotificationService::showRecordingStopped(const QString &summary) {
  showNotification("Recording Saved", summary, "trophy", 3000, 2);
}

QString NotificationService::formatDuration(qint64 ms) {
  if (ms < 0)
    ms = 0;
  const qint64 s = ms / 1000;
  const qint64 h = s / 3600;
  const qint64 m = (s % 3600) / 60;
  const qint64 sec = s % 60;
  if (h > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(h)
        .arg(m, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0'));
  }
  return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

void NotificationService::showError(const QString &message) {
  showNotification("Recording Error", message, "dialog-error", 5000, 2);
}

} // namespace TrophyBackend
