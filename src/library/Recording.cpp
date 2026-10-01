/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Recording.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>

extern "C" {
#include <libavformat/avformat.h>
}

namespace TrophyBackend {

Recording::Recording(const QString &filePath, QObject *parent)
    : QObject(parent), m_filePath(filePath) {
  QFileInfo fileInfo(filePath);
  m_fileName = fileInfo.fileName();
  m_directory = fileInfo.absolutePath();
  m_size = fileInfo.size();
  m_date = fileInfo.birthTime();

  if (!m_date.isValid()) {
    m_date = fileInfo.lastModified();
  }

  // Check for existing thumbnail
  QString thumbPath =
      m_directory + "/.thumbnails/" + fileInfo.completeBaseName() + ".jpg";
  if (QFile::exists(thumbPath)) {
    m_thumbnailPath = thumbPath;
  }
}

Recording::~Recording() = default;

void Recording::setThumbnailPath(const QString &path) {
  if (m_thumbnailPath != path) {
    m_thumbnailPath = path;
    emit thumbnailPathChanged();
  }
}

void Recording::loadMetadata() {
  parseFileNameForGameName();

  // Run probing in a background thread to prevent UI freezing.
  // Guarded by QPointer: the Recording may be destroyed (e.g. library
  // refresh during shutdown) before the probe finishes.
  QPointer<Recording> self(this);
  QThreadPool::globalInstance()->start([self]() {
    if (self.isNull())
      return;
    self->probeVideoFile();
    if (self.isNull())
      return;
    // Notify on the main thread
    QMetaObject::invokeMethod(self.data(), &Recording::metadataChanged,
                              Qt::QueuedConnection);
  });
}

void Recording::parseFileNameForGameName() {
  // Expected format: GameName_YYYY-MM-DD_HH-mm-ss.ext
  // or: Recording_YYYY-MM-DD_HH-mm-ss.ext

  QFileInfo fileInfo(m_filePath);
  QString baseName = fileInfo.completeBaseName();

  // Try to extract game name (everything before the timestamp)
  QRegularExpression timestampRe("_\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}$");
  int timestampIndex = baseName.indexOf(timestampRe);

  if (timestampIndex > 0) {
    m_gameName = baseName.left(timestampIndex);
    // Replace underscores with spaces for display
    m_gameName.replace('_', ' ');
  } else {
    m_gameName = baseName;
  }

  // Handle "Recording" as generic name
  if (m_gameName == "Recording") {
    m_gameName = QObject::tr("Unknown Game");
  }
}

void Recording::probeVideoFile() {
  AVFormatContext *formatCtx = nullptr;

  // Skip probing if the file is too small (likely currently being written)
  QFileInfo fileInfo(m_filePath);
  if (fileInfo.size() < 4096) {
    // qInfo() << "Skipping probe for small/new file:" << m_filePath;
    return;
  }

  int ret = avformat_open_input(&formatCtx, m_filePath.toUtf8().constData(),
                                nullptr, nullptr);
  if (ret < 0) {
    // Only warn if the file isn't tiny
    if (fileInfo.size() > 1024 * 1024) {
      qWarning() << "Failed to open video file for probing:" << m_filePath;
    }
    return;
  }

  // Read container metadata tags first. These are more reliable than
  // filename parsing for game title and recording date — unless the user
  // renamed the file, in which case the embedded title is stale and the
  // filename is authoritative (creation_time stays valid regardless).
  if (formatCtx->metadata) {
    if (!m_preferFileNameTitle) {
      AVDictionaryEntry *titleTag =
          av_dict_get(formatCtx->metadata, "title", nullptr, 0);
      QString containerTitle;
      if (titleTag && titleTag->value)
        containerTitle = QString::fromUtf8(titleTag->value).trimmed();

      if (containerTitle.isEmpty()) {
        titleTag = av_dict_get(formatCtx->metadata, "comment", nullptr, 0);
        if (titleTag && titleTag->value)
          containerTitle = QString::fromUtf8(titleTag->value).trimmed();
      }

      if (!containerTitle.isEmpty()) {
        m_gameName = containerTitle;
      }
    }

    AVDictionaryEntry *tag =
        av_dict_get(formatCtx->metadata, "creation_time", nullptr, 0);
    if (tag && tag->value) {
      QDateTime creationTime = QDateTime::fromString(
          QString::fromUtf8(tag->value).trimmed(), Qt::ISODate);
      if (creationTime.isValid()) {
        m_date = creationTime;
      }
    }
  }

  // Extract resolution even if stream info hasn't been called (often in header)
  for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
    AVStream *stream = formatCtx->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      m_width = stream->codecpar->width;
      m_height = stream->codecpar->height;
      break;
    }
  }

  // Only call find_stream_info if we need duration and it's missing from header
  if (formatCtx->duration == AV_NOPTS_VALUE) {
    // We increase probesize to avoid the warning, and also set max_analyze_duration
    formatCtx->probesize = 5 * 1024 * 1024; // 5MB
    formatCtx->max_analyze_duration = 5 * AV_TIME_BASE; // 5 seconds
    avformat_find_stream_info(formatCtx, nullptr);
  }

  // Get duration
  if (formatCtx->duration != AV_NOPTS_VALUE) {
    m_duration = formatCtx->duration / AV_TIME_BASE;
  }

  avformat_close_input(&formatCtx);
}

} // namespace TrophyBackend
