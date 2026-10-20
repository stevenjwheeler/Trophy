/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QPointer>
#include <QTimer>

namespace TrophyBackend {

/**
 * @brief Checks GitHub Releases for a newer Trophy version
 *
 * Queries the repo's latest stable release (drafts and
 * prereleases are excluded by the API) and compares its tag
 * against the running build's version. Branch builds
 * (`<base>-<branch>-<sha>`) never compare: a dev build would
 * always read as "behind" the nearest tag, which would nag
 * constantly with no actionable update.
 *
 * The check is opt-out (updateCheckEnabled, default on), runs
 * at most once a day, and is scheduled ~60s after startup so
 * it never races launch. Network errors are logged only — a
 * recorder must never nag about connectivity.
 */
class UpdateChecker : public QObject {
  Q_OBJECT
  // Set by Application before the first check (mirrors how
  // AppVersion reaches QML). Empty disables checking entirely.
  Q_PROPERTY(QString currentVersion READ currentVersion WRITE
                 setCurrentVersion NOTIFY currentVersionChanged)

public:
  explicit UpdateChecker(QObject *parent = nullptr);
  ~UpdateChecker() override;

  QString currentVersion() const { return m_currentVersion; }
  void setCurrentVersion(const QString &version);

  // Release tag -> comparable parts. Pure + static for unit
  // tests (no network involved). Returns false for non-release
  // shapes (branch stamps, empty, garbage).
  static bool parseVersion(const QString &tag, int *major,
                            int *minor, int *patch);
  // Three-way compare of parsed versions: 1 = a newer,
  // 0 = equal, -1 = older.
  static int compareVersions(int aMajor, int aMinor, int aPatch,
                             int bMajor, int bMinor, int bPatch);

public slots:
  // One-shot check (also used by the Info page's manual
  // "Check for updates"). Emits checkFinished; the daily
  // schedule ignores the result and re-arms.
  void checkNow();

signals:
  void currentVersionChanged();
  void checkFinished(bool newerAvailable, const QString &latestVersion,
                     const QString &releaseUrl);

private slots:
  void onReplyFinished();

private:
  void startRequest();

  QNetworkAccessManager m_network;
  QString m_currentVersion;
  QPointer<QNetworkReply> m_reply;
  // Bounds a hung GET: aborting settles via the normal error path so
  // one wedged request can't suppress checks for the whole session.
  QTimer m_requestTimeout;

  static constexpr const char *kApiUrl =
      "https://api.github.com/repos/stevenjwheeler/Trophy/releases/latest";
};

} // namespace TrophyBackend
