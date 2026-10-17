/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <QUrl>

namespace TrophyBackend {

// Presentation only: never changes detection, whitelist or approval keys.
inline QUrl gameIconSource(QString key, uint steamAppId = 0) {
  key = key.trimmed().toLower();
  if (key.endsWith(".exe"))
    key.chop(4);
  if (steamAppId == 0 && key.startsWith("steam_app_"))
    steamAppId = key.mid(10).toUInt();
  if (key.isEmpty() && steamAppId == 0)
    return {};
  return QUrl(QStringLiteral("image://gameicon/%1/%2")
                  .arg(steamAppId)
                  .arg(QString::fromLatin1(key.toUtf8().toBase64(
                      QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals))));
}

} // namespace TrophyBackend
