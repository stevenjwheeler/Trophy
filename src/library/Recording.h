/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>

namespace TrophyBackend {

/**
 * @brief Represents a single video recording
 *
 * Contains metadata about the recording including file info,
 * game name, duration, and thumbnail path.
 */
class Recording : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString filePath READ filePath CONSTANT)
  Q_PROPERTY(QString fileName READ fileName CONSTANT)
  Q_PROPERTY(QString directory READ directory CONSTANT)
  Q_PROPERTY(QString gameName READ gameName NOTIFY metadataChanged)
  // Produced asynchronously by probeVideoFile: filesystem dates first,
  // then the container tags (trophy-date/time win over creation_time).
  Q_PROPERTY(QDateTime date READ date NOTIFY metadataChanged)
  Q_PROPERTY(qint64 duration READ duration NOTIFY metadataChanged)
  Q_PROPERTY(qint64 size READ size CONSTANT)
  Q_PROPERTY(
      QString thumbnailPath READ thumbnailPath NOTIFY thumbnailPathChanged)
  Q_PROPERTY(int width READ width NOTIFY metadataChanged)
  Q_PROPERTY(int height READ height NOTIFY metadataChanged)
  // Media kind: container tag (trophy-kind) wins, _Replay filename
  // suffix is the fallback for older files. Survives renames.
  Q_PROPERTY(bool isClip READ isClip NOTIFY metadataChanged)
  // Recorder version that wrote the file (trophy-version tag; empty on
  // older files that predate the tag).
  Q_PROPERTY(QString trophyVersion READ trophyVersion NOTIFY metadataChanged)

public:
  explicit Recording(const QString &filePath, QObject *parent = nullptr);
  ~Recording() override;

  QString filePath() const { return m_filePath; }
  QString fileName() const { return m_fileName; }
  QString directory() const { return m_directory; }
  QString gameName() const { return m_gameName; }
  QDateTime date() const { return m_date; }
  qint64 duration() const { return m_duration; } // seconds
  qint64 size() const { return m_size; }         // bytes
  QString thumbnailPath() const { return m_thumbnailPath; }
  int width() const { return m_width; }
  int height() const { return m_height; }
  bool isClip() const { return m_isClip; }
  QString trophyVersion() const { return m_trophyVersion; }

  void setThumbnailPath(const QString &path);

  /**
   * @brief Prefer the filename-derived title over container metadata.
   *
   * Set for user-renamed files: the container still carries the title
   * baked in at record time, so without this the background probe would
   * overwrite the new filename-derived game name with the stale tag.
   * Must be called before loadMetadata().
   */
  void setPreferFileNameTitle(bool prefer) {
    m_preferFileNameTitle = prefer;
  }

  /**
   * @brief Load metadata from the video file
   */
  void loadMetadata();

signals:
  void thumbnailPathChanged();
  void metadataChanged();

private:
  void parseFileNameForGameName();
  void probeVideoFile();

  QString m_filePath;
  QString m_fileName;
  QString m_directory;
  QString m_gameName;
  QDateTime m_date;
  qint64 m_duration = 0;
  qint64 m_size = 0;
  QString m_thumbnailPath;
  int m_width = 0;
  int m_height = 0;
  bool m_preferFileNameTitle = false;
  bool m_isClip = false;
  QString m_trophyVersion;
};

} // namespace TrophyBackend
