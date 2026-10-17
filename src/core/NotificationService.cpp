/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "NotificationService.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
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
  // The authorize prompt is resident; sendNotification also makes explicitly
  // persistent (timeout 0) guidance resident. Transient notices must expire
  // fully — with resident set, Plasma keeps every clip/recording notice until
  // manually dismissed.
  if (authorizeAction)
    hints["resident"] = true;
  return hints;
}

void NotificationService::closeLastNotification() {
  if (m_lastNotificationId == 0) {
    m_pendingWithdrawTitle = m_lastTitle;
    return;
  }
  closeNotificationById(m_lastNotificationId);
  m_lastNotificationId = 0;
}

void NotificationService::closeAuthorizePrompt(const QString &game) {
  if (game.isEmpty())
    return;
  if (m_lastTitle != QStringLiteral("New Game: ") + game)
    return;
  if (m_lastNotificationId == 0) {
    // The Notify reply hasn't arrived yet (async): the id is still
    // unknown, so record the title and let the reply callback withdraw
    // the popup the moment it publishes. Without this, a game closing
    // in the reply round-trip leaves a dead-name prompt behind.
    m_pendingWithdrawTitle = m_lastTitle;
    return;
  }
  closeNotificationById(m_lastNotificationId);
  m_lastNotificationId = 0;
}

void NotificationService::showCaptureSelectionHint(const QString &game) {
  // Replace the authorize prompt in place, without carrying its actions over.
  // Escape markup: notification bodies may be rendered as rich text.
  showNotification(QStringLiteral("Select capture: ") + game,
                   QStringLiteral("Select the “%1” game window in the screen-sharing dialog, then confirm sharing.")
                       .arg(game.toHtmlEscaped()),
                   QStringLiteral("trophy"), 0, 2);
}

void NotificationService::closeCaptureSelectionHint() {
  // Also handles completion before the asynchronous Notify reply arrives.
  // Never withdraw an error/recording notice that has replaced the hint.
  if (m_lastTitle.startsWith(QStringLiteral("Select capture: ")))
    closeLastNotification();
}

void NotificationService::closeNotificationById(uint id) {
  auto call = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "CloseNotification");
  call.setArguments({id});
  // Explicit messages avoid synchronous dynamic-proxy introspection.
  QDBusConnection::sessionBus().asyncCall(call);
  // Deliberately leaves m_lastNotificationId alone: click-matching for a
  // newer prompt (e.g. authorize) must keep working after an older
  // transient notice is withdrawn.
}
void NotificationService::showNotification(const QString &title,
                                           const QString &body,
                                           const QString &icon, int timeout,
                                           int urgency,
                                            bool authorizeAction,
                                            const QString &actionId,
                                            const QString &actionLabel) {
  sendNotification(title, body, icon, timeout, urgency, authorizeAction, true,
                   actionId, actionLabel);
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
  // The dedup stamp was set at enqueue time and this send was already
  // approved above: clear it so the direct send is not suppressed by
  // its own stamp (mirrors drainNoticeQueue).
  m_lastSentMs = -1;
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
  if (isRepeatSuppressed(title, body, m_dedupTitle, m_dedupBody, elapsed)) {
    qDebug() << "Suppressing duplicate notification within cooldown:" << title;
    return true;
  }
  m_dedupTitle = title;
  m_dedupBody = body;
  m_lastSentMs = nowMs;
  return false;
}

quint64 NotificationService::beginReplacingNotification(const QString &title) {
  ++m_notificationGeneration;
  m_replaceTargetId = m_lastNotificationId;
  m_lastTitle = title;
  m_lastNotificationId = 0;
  m_pendingWithdrawTitle.clear();
  return m_notificationGeneration;
}

bool NotificationService::adoptNotificationReply(quint64 generation, uint id) {
  if (generation != m_notificationGeneration)
    return false;
  if (!m_pendingWithdrawTitle.isEmpty() &&
      m_pendingWithdrawTitle == m_lastTitle) {
    m_pendingWithdrawTitle.clear();
    return false;
  }
  m_lastNotificationId = id;
  return true;
}

bool NotificationService::mayWithdrawNotification(quint64 generation,
                                                uint id) const {
  // Notify commonly reuses replaces_id. An old reply/timer cannot close
  // the same id after a successor owns it, including its reply round-trip.
  return generation == m_notificationGeneration ||
         (id != m_lastNotificationId && id != m_replaceTargetId);
}

QString NotificationService::notificationIconPath() {
  // Same extract-once pattern as the chime wavs in Application::playChime:
  // Notify needs a real file, and the resource only exists inside the
  // binary (installed installs and AppImages alike).
  static QString cached;
  if (!cached.isEmpty() && QFileInfo(cached).size() > 0)
    return cached;
  cached.clear();
  const QString dest =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
      QStringLiteral("/trophy/icons/trophy.svg");
  QFileInfo info(dest);
  if (info.size() <= 0) {
    QDir().mkpath(info.path());
    QFile::remove(dest);
    if (!QFile::copy(QStringLiteral(":/icons/trophy.svg"), dest))
      return QString();
  }
  cached = dest;
  return cached;
}

QString NotificationService::resolveNotificationIcon(
    const QString &icon, const QString &extractedPath) {
  if ((icon.isEmpty() || icon == QStringLiteral("trophy")) &&
      !extractedPath.isEmpty())
    return extractedPath;
  return icon.isEmpty() ? QStringLiteral("trophy") : icon;
}

void NotificationService::sendNotification(const QString &title,
                                           const QString &body,
                                           const QString &icon, int timeout,
                                           int urgency, bool authorizeAction,
                                           bool replacePrevious,
                                           const QString &actionId,
                                           const QString &actionLabel) {
  if (!m_enabled) {
    return;
  }

  if (noteSent(title, body)) {
    return;
  }

  QString appIcon = resolveNotificationIcon(icon, notificationIconPath());

  QVariantMap hints = notificationHints(urgency, authorizeAction);
  if (timeout == 0)
    hints["resident"] = true;

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
  // A caller-supplied button (e.g. "View Release" or "Open Trophy").
  // Attention also supports body clicks, without capture authorization.
  if (!authorizeAction && !actionId.isEmpty() &&
      !actionLabel.isEmpty()) {
    actions << actionId << actionLabel;
    if (actionId == QStringLiteral("show-attention"))
      actions << "default" << "";
  }

  // Fire-and-forget: a blocking Notify call here would freeze the GUI thread
  // (and recording controls) while a hung daemon stalls. The reply id is
  // harvested in the watcher callback, where the replace-slot store and
  // the self-dismiss countdown live so behavior is unchanged.
  const uint replacesId = replacePrevious ? m_lastNotificationId : 0u;
  const quint64 generation = replacePrevious
                                 ? beginReplacingNotification(title)
                                 : m_notificationGeneration;
  auto call = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
      "org.freedesktop.Notifications", "Notify");
  call.setArguments({QStringLiteral("Trophy"), replacesId, appIcon, title,
                     body, actions, hints, timeout});
  QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(call);
  auto *watcher = new QDBusPendingCallWatcher(pending, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation, timeout, replacePrevious](QDBusPendingCallWatcher *w) {
            QDBusPendingReply<uint> reply = *w;
            w->deleteLater();
            if (!reply.isValid()) {
              qWarning() << "Failed to send notification:"
                         << reply.error().message();
              return;
            }
            const uint id = reply.value();
            // Only the replace path owns the "last" slot: a stacked popup
            // must never disturb authorize click-matching or
            // closeLastNotification.
            if (replacePrevious) {
              // Stale replies and closed pending prompts cannot reclaim
              // action ownership. A daemon may reuse a replaced id; do not
              // withdraw that id if a newer reply already adopted it.
              if (!adoptNotificationReply(generation, id) &&
                  mayWithdrawNotification(generation, id)) {
                closeNotificationById(id);
              }
            }
            // Self-dismiss transient notices per id: timeout 0 means "stay
            // until acted on" (authorize prompt / capture guidance). Withdrawing
            // an already-replaced id is a daemon-side no-op, so each notice
            // owns its own countdown instead of sharing one timer. The
            // countdown also marks the end of the on-screen window that
            // queued notices wait for.
            if (timeout > 0) {
              ++m_visibleTransients;
              QTimer::singleShot(timeout, this, [this, id, generation,
                                               replacePrevious]() {
                if (!replacePrevious || mayWithdrawNotification(generation, id))
                  closeNotificationById(id);
                if (m_visibleTransients > 0 && --m_visibleTransients == 0 &&
                    !m_noticeQueue.isEmpty()) {
                  // Hold one beat past the withdraw so the slide-out
                  // animation completes before the queued notice arrives
                  // (drainNoticeQueue still no-ops if another transient
                  // appeared meanwhile).
                  QTimer::singleShot(kQueueDrainGraceMs, this,
                                     [this]() { drainNoticeQueue(); });
                }
              });
            }
          });
}

void NotificationService::onActionInvoked(uint id, const QString &action) {
  // The ActionInvoked connection is daemon-wide (all apps): only our own
  // last notification id may emit, and never while idle (id 0 would
  // otherwise fire on any app's action).
  if (id == m_lastNotificationId && m_lastNotificationId != 0) {
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

void NotificationService::showAttention(const QString &reason) {
  if (!m_enabled)
    return;
  if (!m_attentionService) {
    m_attentionService = new NotificationService(this);
    connect(m_attentionService, &NotificationService::notificationClicked,
            this, [this](const QString &action) {
              if (action == QStringLiteral("show-attention") ||
                  action == QStringLiteral("default")) {
                clearAttention();
                emit attentionRequested();
              }
            });
  }
  m_attentionService->showNotification(
      tr("Trophy needs your attention"), reason.toHtmlEscaped(),
      QStringLiteral("trophy"), 0, 1, false,
      QStringLiteral("show-attention"), tr("Open Trophy"));
}

void NotificationService::clearAttention() {
  if (!m_attentionService)
    return;
  m_attentionService->closeLastNotification();
  // A resolved problem followed by the same failure is a new notice.
  m_attentionService->m_lastSentMs = -1;
}

} // namespace TrophyBackend
