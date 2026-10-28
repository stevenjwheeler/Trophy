/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QPointer>
#include <QTimer>

namespace TrophyBackend {

class UpdateInstaller : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
  Q_PROPERTY(qint64 progressBytes READ progressBytes NOTIFY progressChanged)
  Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
  Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
  explicit UpdateInstaller(QObject *parent = nullptr);

  bool busy() const { return m_busy; }
  bool forceFail() const { return m_forceFail; }
  void setForceFail(bool on) { m_forceFail = on; }
  bool recording() const { return m_recording; }
  void setRecording(bool on) { m_recording = on; }
  qint64 progressBytes() const { return m_progress; }
  qint64 totalBytes() const { return m_total; }
  QString status() const { return m_status; }

  // AppImage-only: download `url`, verify SHA-256 `digest` (may be empty to
  // skip), then atomically replace `targetPath` via a helper and relaunch.
  Q_INVOKABLE void installAppImage(const QString &url, const QString &digest,
                                   const QString &targetPath);
  // Testing helper: simulate a download+install with progress, without
  // touching the network or the filesystem. Never quits the app.
  Q_INVOKABLE void simulate();

signals:
  void busyChanged();
  void progressChanged();
  void statusChanged();
  void finished(bool ok, const QString &message);

private:
  void setStatus(const QString &s);
  void setBusy(bool b);
  void fail(const QString &msg);
  void spawnHelperAndQuit(const QString &newFile,
                         const QString &targetPath);

  QNetworkAccessManager m_network;
  QPointer<QNetworkReply> m_reply;
  QTimer m_timeout;

  bool m_busy = false;
  bool m_forceFail = false;
  bool m_recording = false;
  qint64 m_progress = 0;
  qint64 m_total = 0;
  QString m_status;
  QString m_targetPath;
  QString m_tempPath;
  QString m_digest;
};

} // namespace TrophyBackend