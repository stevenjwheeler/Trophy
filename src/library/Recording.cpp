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
#include <QRegularExpression>

extern "C" {
#include <libavformat/avformat.h>
}

namespace GameRecorder {

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
  probeVideoFile();
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

  ret = avformat_find_stream_info(formatCtx, nullptr);
  if (ret < 0) {
    avformat_close_input(&formatCtx);
    return;
  }

  // Get duration
  if (formatCtx->duration != AV_NOPTS_VALUE) {
    m_duration = formatCtx->duration / AV_TIME_BASE;
  }

  // Find video stream for resolution
  for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
    AVStream *stream = formatCtx->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      m_width = stream->codecpar->width;
      m_height = stream->codecpar->height;
      break;
    }
  }

  avformat_close_input(&formatCtx);
}

} // namespace GameRecorder
