/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace TrophyBackend::RecordingSidecars {

inline QDir directory(const QString &videoPath) {
  return QDir(QFileInfo(videoPath).absolutePath() + "/.thumbnails");
}

inline QStringList owners(const QString &name) {
  static const QRegularExpression preview("^(.+)\\.preview-v1-(?:mkv|mp4|webm)-[0-9a-f]{16}\\.mp4$");
  const auto previewMatch = preview.match(name);
  if (previewMatch.hasMatch())
    return {previewMatch.captured(1)};
  if (name.endsWith(".peaks"))
    return {name.chopped(6)};
  if (!name.endsWith(".jpg"))
    return {};
  QStringList result{name.chopped(4)};
  static const QRegularExpression strip("^(.+)\\.strip-[0-9]+(?:-[0-9]+)?\\.jpg$");
  const auto match = strip.match(name);
  if (match.hasMatch())
    result.append(match.captured(1));
  return result;
}

inline QSet<QString> liveStems(const QDir &dir) {
  QSet<QString> result;
  for (const QFileInfo &info : dir.entryInfoList(
           {"*.mkv", "*.mp4", "*.webm"}, QDir::Files))
    result.insert(info.completeBaseName());
  return result;
}

inline void removeFile(const QString &path) {
  if (!QFile::remove(path))
    qWarning() << "Failed to delete recording cache:" << path;
}

inline bool hasVideo(const QDir &dir, const QString &stem) {
  for (const QString &suffix : {QString(".mkv"), QString(".mp4"), QString(".webm")})
    if (QFileInfo(dir.filePath(stem + suffix)).isFile())
      return true;
  return false;
}

// Only recognized cache files in a real .thumbnails directory. Shared stems
// (e.g. game.mkv and game.mp4) and ambiguous strip-like video names stay safe.
inline void removeOrphans(const QString &libraryPath) {
  const QDir cache(QDir(libraryPath).filePath(".thumbnails"));
  if (!cache.exists() || QFileInfo(cache.absolutePath()).isSymLink())
    return;
  const auto live = liveStems(QDir(libraryPath));
  for (const QString &name : cache.entryList(QDir::Files | QDir::NoSymLinks)) {
    const auto candidates = owners(name);
    if (candidates.isEmpty())
      continue;
    bool keep = false;
    for (const QString &stem : candidates)
      keep |= live.contains(stem);
    if (!keep)
      removeFile(cache.filePath(name));
  }
}

inline void removeForVideo(const QString &videoPath) {
  const QDir cache = directory(videoPath);
  if (!cache.exists() || QFileInfo(cache.absolutePath()).isSymLink())
    return;
  const QString stem = QFileInfo(videoPath).completeBaseName();
  const QDir videos(QFileInfo(videoPath).absolutePath());
  for (const QString &name : cache.entryList(QDir::Files | QDir::NoSymLinks)) {
    const auto candidates = owners(name);
    if (!candidates.contains(stem))
      continue;
    bool keep = false;
    for (const QString &owner : candidates)
      keep |= hasVideo(videos, owner);
    if (!keep)
      removeFile(cache.filePath(name));
  }
}

inline void renameForVideo(const QString &oldPath, const QString &newPath) {
  const QDir cache = directory(oldPath);
  if (!cache.exists() || QFileInfo(cache.absolutePath()).isSymLink())
    return;
  const QString oldStem = QFileInfo(oldPath).completeBaseName();
  const QString newStem = QFileInfo(newPath).completeBaseName();
  if (oldStem == newStem)
    return;
  const QDir videos(QFileInfo(oldPath).absolutePath());
  for (const QString &name : cache.entryList(QDir::Files | QDir::NoSymLinks)) {
    const auto candidates = owners(name);
    if (!candidates.contains(oldStem))
      continue;
    bool shared = false;
    for (const QString &owner : candidates)
      shared |= hasVideo(videos, owner);
    if (shared)
      continue; // The new video can regenerate; don't steal another's cache.
    const QString destination = cache.filePath(newStem + name.mid(oldStem.size()));
    if (QFile::exists(destination))
      removeFile(cache.filePath(name));
    else if (!QFile::rename(cache.filePath(name), destination))
      qWarning() << "Failed to rename recording cache:" << name;
  }
}

} // namespace TrophyBackend::RecordingSidecars
