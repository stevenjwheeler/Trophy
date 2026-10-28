/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace TrophyBackend::DetectorVersion {
// Zero means unknown, not an inferred version of an old or unreadable helper.
inline int integer(const QString &text) {
  if (!QRegularExpression("^[1-9][0-9]{0,8}$").match(text).hasMatch()) return 0;
  bool ok = false;
  const int value = text.toInt(&ok);
  return ok ? value : 0;
}

inline QByteArray read(const QString &path, qint64 limit) {
  const QFileInfo info(path);
  if (!info.isFile() || info.size() <= 0 || info.size() > limit) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const QByteArray bytes = file.read(limit + 1);
  return file.error() == QFileDevice::NoError && bytes.size() <= limit ? bytes : QByteArray();
}

inline int metadata(const QString &path, bool kwin) {
  auto object = QJsonDocument::fromJson(read(path, 16384)).object();
  if (kwin) {
    object = object.value("KPlugin").toObject();
    if (object.value("Id").toString() != "trophy-monitor") return 0;
    return integer(object.value("Version").toString());
  }
  if (object.value("uuid").toString() != "trophy-monitor@trophy") return 0;
  const auto version = object.value("version");
  if (!version.isDouble()) return 0;
  const double number = version.toDouble();
  if (number < 1 || number > 999999999 || number != static_cast<int>(number)) return 0;
  return static_cast<int>(number);
}

inline int reporter(const QString &path, const QByteArray &helper) {
  // Inspect data only: never dlopen a reporter in the app to ask its version.
  const QByteArray bytes = read(path, 16 * 1024 * 1024);
  if (!bytes.startsWith(QByteArray("\x7f" "ELF", 4))) return 0;
  const QByteArray prefix = "TROPHY_HELPER_VERSION:" + helper + ':';
  const auto begin = bytes.indexOf(prefix);
  if (begin < 0) return 0;
  const auto start = begin + prefix.size();
  const auto end = bytes.indexOf('\0', start);
  if (end < start || end - start > 9) return 0;
  return integer(QString::fromLatin1(bytes.mid(start, end - start)));
}
} // namespace TrophyBackend::DetectorVersion
