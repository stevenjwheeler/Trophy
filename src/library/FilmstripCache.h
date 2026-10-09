/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace TrophyBackend {

/**
 * @brief Evenly-spaced video thumbnails for the player timeline
 *
 * Grabs one small frame per slice of the file (keyframe
 * positions, like ThumbnailGenerator) for the video lane's
 * filmstrip. Work runs on the global thread pool; results
 * arrive via filmstripReady. Thumbs live next to the
 * thumbnails (`.thumbnails/<base>.strip-<i>.jpg`) and are
 * reused while newer than the video itself, so reopening a
 * file is instant.
 */
class FilmstripCache : public QObject {
  Q_OBJECT

public:
  explicit FilmstripCache(QObject *parent = nullptr);
  ~FilmstripCache() override = default;

  // Async thumbs for the file (empty list on failure, so QML
  // never waits forever). Count is caller-chosen; the player
  // uses a fixed strip count.
  Q_INVOKABLE void requestThumbs(const QString &filePath, int count);

signals:
  void filmstripReady(const QString &filePath,
                      const QStringList &thumbPaths);

private:
  QSet<QString> m_pending;
};

} // namespace TrophyBackend
