/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "UpdateChecker.h"

#include "InstallInfo.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cstdio>

namespace TrophyBackend {

UpdateChecker::UpdateChecker(QObject *parent)
    : QObject(parent), m_network(this) {
  m_network.setRedirectPolicy(
      QNetworkRequest::NoLessSafeRedirectPolicy);
  // A hung GET must not wedge checks for the session: aborting the
  // reply drives finished(), so the normal error path settles it.
  m_requestTimeout.setSingleShot(true);
  m_requestTimeout.setInterval(15000);
  connect(&m_requestTimeout, &QTimer::timeout, this, [this]() {
    if (m_reply) {
      m_reply->abort();
    }
  });
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
                                    int aPatch, int bMajor, int bMinor,
                                    int bPatch) {
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

QString UpdateChecker::selectAssetUrl(const QJsonObject &root,
                                       QString *outFileName,
                                       QString *outDigest) const {
  if (outFileName)
    *outFileName = QString();
  if (outDigest)
    *outDigest = QString();

  const QJsonArray assets = root.value("assets").toArray();
  QString wantedSuffix;
  switch (m_installKind) {
  case InstallKind::AppImage:
    wantedSuffix = QStringLiteral(".AppImage");
    break;
  case InstallKind::DebianPackage:
    wantedSuffix = QStringLiteral(".deb");
    break;
  case InstallKind::RpmPackage:
    wantedSuffix = QStringLiteral(".rpm");
    break;
  case InstallKind::PortablePrefix:
    wantedSuffix = QStringLiteral(".tar.xz");
    break;
  default:
    return QString();
  }

  for (const QJsonValue &v : assets) {
    const QJsonObject a = v.toObject();
    const QString name = a.value("name").toString();
    if (!name.endsWith(wantedSuffix))
      continue;
    if (outFileName)
      *outFileName = name;
    if (outDigest)
      *outDigest = a.value("digest").toString();
    return a.value("browser_download_url").toString();
  }
  return QString();
}

void UpdateChecker::checkNow() {
  if (m_currentVersion.isEmpty() && !m_forceFakeUpdate) {
    emit checkFinished(false, QString(), QString());
    emit detailsReady(QString(), QString(), QString());
    return;
  }
  if (m_reply) {
    return;
  }
  if (m_forceFakeUpdate) {
    const QString ver = m_fakeUpdateVersion;
    QString assetName;
    switch (m_installKind) {
    case InstallKind::DebianPackage:
        assetName = QStringLiteral("Trophy-%1-amd64.deb").arg(ver);
        break;
    case InstallKind::RpmPackage:
        assetName = QStringLiteral("Trophy-%1-x86_64.rpm").arg(ver);
        break;
    case InstallKind::PortablePrefix:
        assetName = QStringLiteral("Trophy-%1-Linux.tar.xz").arg(ver);
        break;
    default:
        assetName = QStringLiteral("Trophy-%1-x86_64.AppImage").arg(ver);
        break;
    }
    m_latestAssetUrl =
        QStringLiteral("https://github.com/stevenjwheeler/Trophy/releases/download/v%1/%2")
            .arg(ver, assetName);
    m_latestAssetFileName = assetName;
    m_latestDigest.clear();
    m_latestReleaseNotes =
        QStringLiteral("## What's new in %1\n\n- Fake changelog entry one.\n"
                       "- Fake changelog entry two.")
            .arg(ver);
    emit latestReleaseNotesChanged();
    emit checkFinished(true, QStringLiteral("v%1").arg(ver),
                      QStringLiteral(
                          "https://github.com/stevenjwheeler/Trophy/releases/tag/v%1")
                          .arg(ver));
    emit detailsReady(m_latestAssetUrl, m_latestAssetFileName,
                      m_latestDigest);
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
  m_requestTimeout.start();
}

void UpdateChecker::onReplyFinished() {
  m_requestTimeout.stop();
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
    qWarning("Update check failed: %s",
             reply->errorString().toUtf8().constData());
    emit checkFailed();
    return;
  }

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
    qWarning("Update check: non-release tag shape (running '%s', "
             "latest '%s')",
             m_currentVersion.toUtf8().constData(),
             tagName.toUtf8().constData());
    emit checkFailed();
    return;
  }

  latestVersion = tagName;
  if (compareVersions(rMajor, rMinor, rPatch, cMajor, cMinor,
                      cPatch) > 0) {
    newer = true;
    qDebug("Update available: %s (running %s)",
           tagName.toUtf8().constData(),
           m_currentVersion.toUtf8().constData());
  }
  QString fn, digest;
  const QString url = selectAssetUrl(root, &fn, &digest);
  m_latestAssetUrl = url;
  m_latestAssetFileName = fn;
  m_latestDigest = digest;
  m_latestReleaseNotes = root.value("body").toString();
  emit latestReleaseNotesChanged();

  emit checkFinished(newer, latestVersion, releaseUrl);
  emit detailsReady(m_latestAssetUrl, m_latestAssetFileName,
                    m_latestDigest);
}

} // namespace TrophyBackend