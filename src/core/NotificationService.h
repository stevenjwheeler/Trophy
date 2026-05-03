/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

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
                                    int timeout = 5000, int urgency = 1);

  Q_INVOKABLE void showRecordingStarted(const QString &gameName);
  Q_INVOKABLE void showRecordingStopped(const QString &filePath);
  Q_INVOKABLE void showError(const QString &message);

signals:
  void enabledChanged();
  void notificationClicked(const QString &action);

private slots:
  void onActionInvoked(uint id, const QString &action);
  void onNotificationClosed(uint id, uint reason);

private:
  bool m_enabled = true;
  uint m_lastNotificationId = 0;
};

} // namespace TrophyBackend
