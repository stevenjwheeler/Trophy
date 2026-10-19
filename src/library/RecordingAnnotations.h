/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVariantList>
#include <QDateTime>
#include "audio/AudioRoutingInfo.h"
#include <cerrno>
#include <sys/stat.h>

namespace TrophyBackend::RecordingAnnotations {
// User data, NOT a regenerable thumbnail cache. Include the video extension
// so same-stem MKV/MP4 files never share protection or bookmarks.
inline QString path(const QString &video) {
  const QFileInfo info(video);
  return QDir(info.absolutePath()).filePath(".trophy/" + info.fileName() + ".json");
}
inline bool safe(const QString &video) {
  const QFileInfo dir(QFileInfo(path(video)).absolutePath());
  return !dir.isSymLink() && !QFileInfo(path(video)).isSymLink();
}
inline bool read(const QString &video, QJsonObject &object) {
  object = {};
  if (!safe(video)) return false;
  QFile file(path(video));
  struct stat status {};
  if (::lstat(QFile::encodeName(file.fileName()).constData(), &status) != 0)
    return errno == ENOENT;
  // Distinguish absence from permission errors, and never open a FIFO/device
  // (which could block the UI or the cleanup pass indefinitely).
  if (!S_ISREG(status.st_mode) || status.st_size > 1024 * 1024) return false;
  if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return false;
  const auto bytes = file.read(1024 * 1024 + 1);
  if (file.error() != QFileDevice::NoError || bytes.size() > 1024 * 1024) return false;
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
  object = document.object();
  if ((object.contains("protected") && !object.value("protected").isBool()) ||
      (object.contains("bookmarks") && !object.value("bookmarks").isArray())) return false;
  const auto marks = object.value("bookmarks").toArray();
  if (marks.size() > 1000) return false;
  for (const auto &entry : marks) {
    if (!entry.isObject()) return false;
    const auto mark = entry.toObject();
    const double ms = mark.value("positionMs").toDouble(-1);
    if (!(ms >= 0 && ms <= 86400000) || ms != qint64(ms)) return false;
    if (mark.contains("label") && !mark.value("label").isString()) return false;
  }
  return true;
}
inline bool write(const QString &video, const QJsonObject &object) {
  if (!safe(video) || !QFileInfo(video).isFile()) return false;
  if (!QDir().mkpath(QFileInfo(path(video)).absolutePath())) return false;
  QSaveFile file(path(video));
  const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
  if (bytes.size() > 1024 * 1024) return false;
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
inline bool isProtected(const QString &video) {
  QJsonObject object;
  // Fail closed: unreadable user data must never result in auto-deletion.
  return !read(video, object) || object.value("protected").toBool();
}
inline bool setProtected(const QString &video, bool value) {
  QJsonObject object;
  if (!read(video, object)) return false;
  object["protected"] = value;
  return write(video, object);
}
inline QJsonObject audioRouting(const QString &video, const QJsonObject &object) {
  const auto routing = object.value("audioRouting").toObject();
  if (routing.isEmpty()) return {};
  const QFileInfo file(video);
  if (routing.value("sourceSize").toDouble(-1) != file.size() ||
      routing.value("sourceMtimeMs").toDouble(-1) != file.lastModified().toMSecsSinceEpoch()) return {};
  return AudioRoutingInfo::fromJson(routing).toJson();
}
inline QJsonObject audioRouting(const QString &video) {
  QJsonObject object;
  return read(video, object) ? audioRouting(video, object) : QJsonObject{};
}
inline bool setAudioRouting(const QString &video, QJsonObject routing) {
  if (routing.isEmpty()) return true;
  if (!AudioRoutingInfo::fromJson(routing).known) return false;
  QJsonObject object;
  if (!read(video, object)) return false;
  const QFileInfo file(video);
  routing["sourceSize"] = file.size();
  routing["sourceMtimeMs"] = file.lastModified().toMSecsSinceEpoch();
  object["audioRouting"] = routing;
  return write(video, object);
}
inline QVariantList bookmarks(const QString &video) {
  QJsonObject object;
  if (!read(video, object)) return {};
  QVariantList result;
  for (const auto &entry : object.value("bookmarks").toArray()) {
    const auto mark = entry.toObject();
    const double ms = mark.value("positionMs").toDouble(-1);
    if (ms >= 0 && ms <= 86400000 && ms == qint64(ms))
      result.append(mark.toVariantMap());
  }
  return result;
}
inline bool addBookmark(const QString &video, qint64 ms, const QString &label = {}) {
  if (ms < 0 || ms > 86400000) return false;
  QJsonObject object;
  if (!read(video, object)) return false;
  auto marks = object.value("bookmarks").toArray();
  // Idempotent for every caller (player and live recording). Check before
  // the capacity limit, and preserve the original label without rewriting.
  for (const auto &entry : marks)
    if (entry.toObject().value("positionMs").toDouble(-1) == ms) return true;
  if (marks.size() >= 1000) return false;
  marks.append(QJsonObject{{"positionMs", ms}, {"label", label.left(120)}});
  object["bookmarks"] = marks;
  return write(video, object);
}
inline bool removeBookmark(const QString &video, int index) {
  QJsonObject object;
  if (!read(video, object)) return false;
  auto marks = object.value("bookmarks").toArray();
  if (index < 0 || index >= marks.size()) return false;
  marks.removeAt(index);
  object["bookmarks"] = marks;
  return write(video, object);
}
inline void remove(const QString &video) {
  if (safe(video)) QFile::remove(path(video));
}
inline bool prepareRename(const QString &from, const QString &to) {
  if (!safe(from) || !safe(to)) return false;
  struct stat status {};
  if (::lstat(QFile::encodeName(path(to)).constData(), &status) == 0 || errno != ENOENT)
    return false;
  if (::lstat(QFile::encodeName(path(from)).constData(), &status) != 0)
    return errno == ENOENT;
  if (!S_ISREG(status.st_mode)) return false;
  // Copy first, remove the old metadata only AFTER the video rename. Even
  // a crash between the two operations cannot leave the video unprotected.
  QFile input(path(from));
  if (!QFileInfo(input).isFile()) return false;
  if (!input.open(QIODevice::ReadOnly) || input.size() > 1024 * 1024) return false;
  const auto bytes = input.read(1024 * 1024 + 1);
  if (input.error() != QFileDevice::NoError || bytes.size() > 1024 * 1024) return false;
  if (!QDir().mkpath(QFileInfo(path(to)).absolutePath())) return false;
  QSaveFile output(path(to));
  return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
} // namespace TrophyBackend::RecordingAnnotations
