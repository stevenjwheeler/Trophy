/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

class QTimer;

namespace TrophyBackend {

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
                                    bool authorizeAction = false);

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

  /** Hint map for a notice. Static + public for tests (no D-Bus needed). */
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
  // Single self-dismiss timer, restarted by every transient notice. Since
  // each Notify replaces the previous one (replaces_id), one countdown
  // tracking the latest notice is exactly right: "Saving…" can never be
  // killed from under "Saved" — the latest send always owns the timer.
  QTimer *m_dismissTimer = nullptr;
  uint m_dismissId = 0;
};

} // namespace TrophyBackend
