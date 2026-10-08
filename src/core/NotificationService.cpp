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

void NotificationService::closeAuthorizePrompt(const QString &game) {
  if (m_lastNotificationId == 0 || game.isEmpty())
    return;
  if (m_lastTitle != QStringLiteral("New Game: ") + game)
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
  sendNotification(title, body, icon, timeout, urgency, authorizeAction, true);
}

void NotificationService::showQueuedNotification(
    const QString &title, const QString &body, const QString &icon,
    int timeout, int urgency) {
  if (!m_enabled)
    return;
  if (noteSent(title, body))
    return;
  // Coalesce with an identical entry already waiting (e.g. the
  // launcher-PID handoff re-arms the DVR seconds after the first arm).
  for (const QueuedNotice &queued : m_noticeQueue) {
    if (queued.title == title && queued.body == body) {
      qDebug() << "Coalescing queued duplicate notification:" << title;
      return;
    }
  }
  if (m_visibleTransients > 0) {
    m_noticeQueue.append({title, body, icon, timeout, urgency});
    return;
  }
  sendNotification(title, body, icon, timeout, urgency, false, false);
}

void NotificationService::drainNoticeQueue() {
  if (m_noticeQueue.isEmpty() || m_visibleTransients > 0)
    return;
  const QueuedNotice next = m_noticeQueue.takeFirst();
  // The dedup stamp was set when the notice was enqueued; the wait likely
  // kept it inside the cooldown, so clear it — this send was already
  // approved above.
  m_lastSentMs = -1;
  sendNotification(next.title, next.body, next.icon, next.timeout,
                   next.urgency, false, false);
}

bool NotificationService::noteSent(const QString &title, const QString &body) {
  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
  const qint64 elapsed = m_lastSentMs < 0 ? -1 : (nowMs - m_lastSentMs);
  if (isRepeatSuppressed(title, body, m_lastTitle, m_lastBody, elapsed)) {
    qDebug() << "Suppressing duplicate notification within cooldown:" << title;
    return true;
  }
  m_lastTitle = title;
  m_lastBody = body;
  m_lastSentMs = nowMs;
  return false;
}

void NotificationService::sendNotification(const QString &title,
                                           const QString &body,
                                           const QString &icon, int timeout,
                                           int urgency, bool authorizeAction,
                                           bool replacePrevious) {
  if (!m_enabled) {
    return;
  }

  if (noteSent(title, body)) {
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

  const QVariantMap hints = notificationHints(urgency, authorizeAction);

  QStringList actions;
  // Action buttons belong only to the authorize prompt — they used to
  // ride along with any urgency>1 notification, which would have put an
  // "Authorize Capture" button on critical recording/clip notices.
  // Approve starts capture (portal token follows); Block vetoes the game
  // in the Games list; dismissing leaves it on Prompt. The "default"
  // action renders no button: daemons invoke it on body-click, so
  // clicking the prompt itself counts as authorize (a bare close still
  // means dismiss — the spec cannot tell X/timeout apart from an
  // action-less body click, and daemons without default-action support
  // fall back to that).
  if (authorizeAction) {
    actions << "authorize" << "Authorize Capture" << "block" << "Block"
            << "default" << "";
  }

  QDBusReply<uint> reply =
      notify.call("Notify",
                  "Trophy", // app_name (keep it short)
                  replacePrevious ? m_lastNotificationId
                                  : 0u, // replaces_id (0 = stack alongside)
                  appIcon,              // app_icon
                  title,                // summary
                  body,                 // body
                  actions,              // actions
                  hints,                // hints
                  timeout               // timeout (ms)
      );

  if (reply.isValid()) {
    const uint id = reply.value();
    // Only the replace path owns the "last" slot: a stacked popup must
    // never disturb authorize click-matching or closeLastNotification.
    if (replacePrevious)
      m_lastNotificationId = id;
    // Self-dismiss transient notices per id: timeout 0 means "stay until
    // acted on" (the authorize prompt). Withdrawing an already-replaced
    // id is a daemon-side no-op, so each notice owns its own countdown
    // instead of sharing one timer. The countdown also marks the end of
    // the on-screen window that queued notices wait for.
    if (timeout > 0) {
      ++m_visibleTransients;
      QTimer::singleShot(timeout, this, [this, id]() {
        closeNotificationById(id);
        if (m_visibleTransients > 0 && --m_visibleTransients == 0 &&
            !m_noticeQueue.isEmpty()) {
          // Hold one beat past the withdraw so the slide-out animation
          // completes before the queued notice arrives (drainNoticeQueue
          // still no-ops if another transient appeared meanwhile).
          QTimer::singleShot(kQueueDrainGraceMs, this,
                             [this]() { drainNoticeQueue(); });
        }
      });
    }
  } else {
    qWarning() << "Failed to send notification:" << reply.error().message();
  }
}

void NotificationService::onActionInvoked(uint id, const QString &action) {
  if (id == m_lastNotificationId || m_lastNotificationId == 0) {
    QString resolved = action;
    // Body-clicking the authorize prompt invokes the "default" action on
    // supporting daemons: that counts as authorize, same as the button.
    if (resolved == QStringLiteral("default") &&
        m_lastTitle.startsWith(QStringLiteral("New Game: ")))
      resolved = QStringLiteral("authorize");
    emit notificationClicked(resolved);
  }
}

void NotificationService::onNotificationClosed(uint id, uint reason) {
  Q_UNUSED(reason);
  // Dismissing the authorize prompt (timeout, X, body-click — all close
  // without ActionInvoked) means "leave it on Prompt": report a
  // dismissal so the game is neither allow-listed nor blocked and no
  // capture starts. Other notifications are ignored here (Application
  // no-ops without pending authorize anyway, but keep this narrow so a
  // stray close can't arm anything unexpected).
  const bool isAuthorize =
      m_lastTitle.startsWith(QStringLiteral("New Game: "));
  if (id == m_lastNotificationId && m_lastNotificationId != 0) {
    m_lastNotificationId = 0;
    if (isAuthorize)
      emit notificationClicked(QStringLiteral("dismissed"));
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
