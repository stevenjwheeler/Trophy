/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFuture>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QString>
#include <atomic>

namespace TrophyBackend {

/**
 * @brief Generates thumbnails for video recordings
 *
 * Uses FFmpeg to extract a frame from videos and save as JPEG thumbnail.
 */
class ThumbnailGenerator : public QObject {
  Q_OBJECT

public:
  explicit ThumbnailGenerator(QObject *parent = nullptr);
  ~ThumbnailGenerator() override;

  /**
   * @brief Request thumbnail generation for a video
   * @param videoPath Path to video file
   */
  void generateThumbnail(const QString &videoPath);

signals:
  void thumbnailReady(const QString &videoPath, const QString &thumbnailPath);
  void thumbnailFailed(const QString &videoPath, const QString &error);

private slots:
  void processQueue();

private:
  bool generateThumbnailSync(const QString &videoPath,
                             const QString &outputPath);
  QString getThumbnailPath(const QString &videoPath) const;

  QQueue<QString> m_queue;
  QMutex m_mutex;
  bool m_processing = false;
  std::atomic<bool> m_terminating = {false};
  QFuture<void> m_future;
};

} // namespace TrophyBackend
