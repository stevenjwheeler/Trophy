/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "UpdateChecker.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cstdio>

namespace TrophyBackend {

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_network(this) {
  m_network.setRedirectPolicy(
      QNetworkRequest::NoLessSafeRedirectPolicy);
}

UpdateChecker::~UpdateChecker() {
  if (m_reply) {
    m_reply->abort();
    m_reply->deleteLater();
  }
}

void UpdateChecker::setCurrentVersion(const QString &version) {
  if (m_currentVersion != version) {
    m_currentVersion = version;
    emit currentVersionChanged();
  }
}

bool UpdateChecker::parseVersion(const QString &tag, int *major,
                                  int *minor, int *patch) {
  // Release tags are vX.Y.Z (the CI scheme stamps the tag on
  // main). Branch stamps read "<base>-<branch>-<sha>" and must
  // not compare against a release tag — a dev build is not
  // "behind" its base tag, it's just not a release.
  static const QRegularExpression re(
      "^v?(\\d+)\\.(\\d+)\\.(\\d+)$");
  const auto m = re.match(tag.trimmed());
  if (!m.hasMatch()) {
    return false;
  }
  if (major)
    *major = m.captured(1).toInt();
  if (minor)
    *minor = m.captured(2).toInt();
  if (patch)
    *patch = m.captured(3).toInt();
  return true;
}

int UpdateChecker::compareVersions(int aMajor, int aMinor,
                                    int aPatch, int bMajor,
                                    int bMinor, int bPatch) {
  const int a[3] = {aMajor, aMinor, aPatch};
  const int b[3] = {bMajor, bMinor, bPatch};
  for (int i = 0; i < 3; ++i) {
    if (a[i] > b[i])
      return 1;
    if (a[i] < b[i])
      return -1;
  }
  return 0;
}

void UpdateChecker::checkNow() {
  if (m_currentVersion.isEmpty()) {
    return;
  }
  if (m_reply) {
    // A check is already in flight; remember the extra caller
    // so its signal isn't swallowed (one reply, one finish).
    m_pendingManualCheck = true;
    return;
  }
  startRequest();
}

void UpdateChecker::startRequest() {
  const QUrl url(kApiUrl);
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    "Trophy UpdateChecker");
  m_reply = m_network.get(request);
  connect(m_reply, &QNetworkReply::finished, this,
          &UpdateChecker::onReplyFinished);
}

void UpdateChecker::onReplyFinished() {
  QPointer<QNetworkReply> reply = m_reply;
  m_reply = nullptr;
  if (!reply) {
    return;
  }
  reply->deleteLater();

  bool newer = false;
  QString latestVersion;
  QString releaseUrl;

  if (reply->error() != QNetworkReply::NoError) {
    // Offline / rate-limited / DNS hiccup: log only. A game
    // recorder must never nag about connectivity.
    qDebug("Update check failed: %s",
           reply->errorString().toUtf8().constData());
  } else {
    const QJsonDocument doc =
        QJsonDocument::fromJson(reply->readAll());
    const QJsonObject root = doc.object();
    const QString tagName = root["tag_name"].toString();
    releaseUrl = root["html_url"].toString();
    int rMajor = 0, rMinor = 0, rPatch = 0;
    int cMajor = 0, cMinor = 0, cPatch = 0;
    if (!parseVersion(tagName, &rMajor, &rMinor, &rPatch) ||
        !parseVersion(m_currentVersion, &cMajor, &cMinor,
                      &cPatch)) {
      // Either side isn't a plain release tag (branch build or
      // an unusual CI stamp) — nothing comparable, stay quiet.
      qDebug("Update check: non-release tag shape (running '%s', "
             "latest '%s')",
             m_currentVersion.toUtf8().constData(),
             tagName.toUtf8().constData());
    } else {
      latestVersion = tagName;
      if (compareVersions(rMajor, rMinor, rPatch, cMajor, cMinor,
                          cPatch) > 0) {
        newer = true;
        qDebug("Update available: %s (running %s)",
               tagName.toUtf8().constData(),
               m_currentVersion.toUtf8().constData());
      }
    }
  }

  // One finish for a coalesced manual check; the daily
  // schedule ignores the payload anyway.
  emit checkFinished(newer, latestVersion, releaseUrl);
}

} // namespace TrophyBackend
