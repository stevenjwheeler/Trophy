/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace TrophyBackend {

class NotificationServiceTest; // test fixture (sets internal state directly)

/**
 * @brief Provides system notification capabilities
 *
 * Uses D-Bus to send notifications via the desktop's notification daemon.
 */
class NotificationService : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
  explicit NotificationService(QObject *parent = nullptr);
  ~NotificationService() override;

  bool isEnabled() const { return m_enabled; }
  void setEnabled(bool enabled);

  Q_INVOKABLE void showNotification(const QString &title, const QString &body,
                                    const QString &icon = QString(),
                                    int timeout = 5000, int urgency = 1,
                                    bool authorizeAction = false,
                                    const QString &actionId = QString(),
                                    const QString &actionLabel = QString());

  /**
   * Queued variant: waits until any currently displayed transient notice
   * has had its full timeout, then appears stacked alongside (never
   * replacing). Used for "Ready to clip", which fires seconds after
   * "Recording Saved" on post-recording DVR resume — sent immediately,
   * the two popups overlap in one visual slot.
   */
  Q_INVOKABLE void showQueuedNotification(const QString &title,
                                          const QString &body,
                                          const QString &icon = QString(),
                                          int timeout = 5000, int urgency = 1);

  Q_INVOKABLE void showRecordingStarted(const QString &gameName);
  Q_INVOKABLE void showRecordingStopped(const QString &summary);
  Q_INVOKABLE void showError(const QString &message);

  /**
   * Withdraw the most recently sent notification, if any. Used for the
   * DVR authorize prompt, which would otherwise linger after the click
   * already armed the capture.
   */
  Q_INVOKABLE void closeLastNotification();

  /**
   * Withdraw one notification by id (no-op when already gone). Used with a
   * timer so transient notices dismiss themselves: Plasma keeps
   * critical-urgency popups up regardless of the requested timeout, and
   * the resident hint is intentionally off for these.
   */
  Q_INVOKABLE void closeNotificationById(uint id);

  /**
   * Withdraw the authorize prompt for one game, if it is the notice
   * currently holding the "last" slot. Title-matched so closing a game
   * can never dismiss an unrelated notice that replaced the prompt.
   */
  void closeAuthorizePrompt(const QString &game);  /** Hint map for a notice. Static + public for tests (no D-Bus needed). */
  static QVariantMap notificationHints(int urgency, bool authorizeAction);
  /** "2:34" / "1:02:03" for notice bodies. Static + public for tests. */
  static QString formatDuration(qint64 ms);

signals:
  void enabledChanged();
  void notificationClicked(const QString &action);

private slots:
  void onActionInvoked(uint id, const QString &action);
  void onNotificationClosed(uint id, uint reason);

public:
  // Duplicate suppression: identical title+body within the cooldown is
  // dropped so burst emitters (e.g. storage auto-cleanup deleting N files)
  // don't trip the daemon's "too many similar notifications" rate limit.
  // Pure predicate for unit testing (no D-Bus involved).
  static bool isRepeatSuppressed(const QString &title, const QString &body,
                                 const QString &lastTitle,
                                 const QString &lastBody, qint64 elapsedMs,
                                 qint64 cooldownMs = 5000);

private:
  bool m_enabled = true;
  uint m_lastNotificationId = 0;
  QString m_lastTitle;
  QString m_lastBody;
  qint64 m_lastSentMs = -1;
  // Shared Notify body. replacePrevious=false sends replaces_id 0 (a
  // separate popup) and leaves m_lastNotificationId alone; the transient
  // self-dismiss is per-notice, since withdrawing an already-replaced id
  // is a harmless no-op on the daemon side.
  void sendNotification(const QString &title, const QString &body,
                        const QString &icon, int timeout, int urgency,
                        bool authorizeAction, bool replacePrevious,
                        const QString &actionId = QString(),
                        const QString &actionLabel = QString());
  // Duplicate check shared by the immediate and queued paths: drops
  // identical title+body within the cooldown, otherwise records this
  // notice as the last sent. Returns true when suppressed.
  bool noteSent(const QString &title, const QString &body);
  // Queued notices waiting for the screen to clear (see
  // showQueuedNotification). Drains one entry once no transient is
  // displayed; the send goes out stacked so a prompt that appeared
  // meanwhile is never clobbered.
  struct QueuedNotice {
    QString title;
    QString body;
    QString icon;
    int timeout = 5000;
    int urgency = 1;
  };
  QList<QueuedNotice> m_noticeQueue;
  int m_visibleTransients = 0;
  void drainNoticeQueue();
  // Extra wait after a withdrawn popup before the queued notice appears,
  // so the first popup's slide-out animation finishes instead of the two
  // visually overlapping.
  static constexpr int kQueueDrainGraceMs = 500;

  friend class NotificationServiceTest;
};

} // namespace TrophyBackend
