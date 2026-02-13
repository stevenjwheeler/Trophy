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

namespace GameRecorder {

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
  Q_PROPERTY(QString gameName READ gameName CONSTANT)
  Q_PROPERTY(QDateTime date READ date CONSTANT)
  Q_PROPERTY(qint64 duration READ duration CONSTANT)
  Q_PROPERTY(qint64 size READ size CONSTANT)
  Q_PROPERTY(
      QString thumbnailPath READ thumbnailPath NOTIFY thumbnailPathChanged)
  Q_PROPERTY(int width READ width CONSTANT)
  Q_PROPERTY(int height READ height CONSTANT)

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

  void setThumbnailPath(const QString &path);

  /**
   * @brief Load metadata from the video file
   */
  void loadMetadata();

signals:
  void thumbnailPathChanged();

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
};

} // namespace GameRecorder
