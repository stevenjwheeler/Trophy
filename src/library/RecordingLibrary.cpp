/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingLibrary.h"
#include "Recording.h"
#include "ThumbnailGenerator.h"

#include <QDebug>
#include <QDir>
#include <algorithm>

namespace GameRecorder {

RecordingLibrary::RecordingLibrary(const QString &libraryPath, QObject *parent)
    : QObject(parent), m_libraryPath(libraryPath),
      m_thumbnailGenerator(std::make_unique<ThumbnailGenerator>(this)) {
  // Connect thumbnail generator
  connect(m_thumbnailGenerator.get(), &ThumbnailGenerator::thumbnailReady, this,
          &RecordingLibrary::onThumbnailReady);

  // Watch for changes
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &RecordingLibrary::onDirectoryChanged);

  // Initial scan
  refresh();
}

RecordingLibrary::~RecordingLibrary() = default;

QList<Recording *> RecordingLibrary::recordings() const {
  return m_filteredRecordings;
}

int RecordingLibrary::count() const { return m_filteredRecordings.size(); }

void RecordingLibrary::setLibraryPath(const QString &path) {
  if (m_libraryPath != path) {
    // Remove old watch
    if (!m_libraryPath.isEmpty() &&
        m_watcher.directories().contains(m_libraryPath)) {
      m_watcher.removePath(m_libraryPath);
    }

    m_libraryPath = path;
    emit libraryPathChanged();
    refresh();
  }
}

void RecordingLibrary::setSearchQuery(const QString &query) {
  if (m_searchQuery != query) {
    m_searchQuery = query;
    updateFilteredRecordings();
  }
}

void RecordingLibrary::setSortOrder(int order) {
  if (m_sortOrder != order) {
    m_sortOrder = order;
    updateFilteredRecordings();
  }
}

void RecordingLibrary::refresh() {
  m_recordings.clear();
  scanDirectory();
  updateFilteredRecordings();
}

Recording *RecordingLibrary::getRecording(const QString &filePath) const {
  for (const auto &rec : m_recordings) {
    if (rec->filePath() == filePath) {
      return rec.get();
    }
  }
  return nullptr;
}

void RecordingLibrary::deleteRecording(const QString &filePath) {
  QFile file(filePath);
  if (file.remove()) {
    removeRecording(filePath);
  } else {
    qWarning() << "Failed to delete recording:" << filePath
               << file.errorString();
  }
}

void RecordingLibrary::setActiveRecordingPath(const QString &path) {
  m_activeRecordingPath = path;
  updateFilteredRecordings();
}

void RecordingLibrary::onDirectoryChanged(const QString &path) {
  Q_UNUSED(path)
  refresh();
}

void RecordingLibrary::onThumbnailReady(const QString &videoPath,
                                        const QString &thumbnailPath) {
  for (auto &rec : m_recordings) {
    if (rec->filePath() == videoPath) {
      rec->setThumbnailPath(thumbnailPath);
      emit recordingsChanged();
      break;
    }
  }
}

void RecordingLibrary::scanDirectory() {
  QDir dir(m_libraryPath);

  if (!dir.exists()) {
    qInfo() << "Library path does not exist:" << m_libraryPath;
    return;
  }

  // Add directory watch
  if (!m_watcher.directories().contains(m_libraryPath)) {
    m_watcher.addPath(m_libraryPath);
  }

  // Scan for video files
  QStringList filters = {"*.mkv", "*.mp4", "*.webm"};
  QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

  qInfo() << "Found" << files.size() << "recordings in" << m_libraryPath;

  for (const QFileInfo &fileInfo : files) {
    addRecording(fileInfo.absoluteFilePath());
  }
}

void RecordingLibrary::addRecording(const QString &filePath) {
  // Skip active recording
  if (filePath == m_activeRecordingPath) {
    return;
  }

  // Check if already exists
  for (const auto &rec : m_recordings) {
    if (rec->filePath() == filePath) {
      return;
    }
  }

  auto recording = std::make_unique<Recording>(filePath, this);
  recording->loadMetadata();

  // Request thumbnail generation
  if (recording->thumbnailPath().isEmpty()) {
    m_thumbnailGenerator->generateThumbnail(filePath);
  }

  m_recordings.push_back(std::move(recording));

  emit recordingAdded(m_recordings.back().get());
}

void RecordingLibrary::removeRecording(const QString &filePath) {
  // Delete thumbnail
  QFileInfo videoInfo(filePath);
  QString thumbDir = videoInfo.absolutePath() + "/.thumbnails";
  QString thumbPath = thumbDir + "/" + videoInfo.completeBaseName() + ".jpg";
  QFile thumbFile(thumbPath);
  if (thumbFile.exists()) {
    if (thumbFile.remove()) {
      qDebug() << "Deleted thumbnail:" << thumbPath;
    } else {
      qWarning() << "Failed to delete thumbnail:" << thumbPath;
    }
  }

  auto it = std::remove_if(m_recordings.begin(), m_recordings.end(),
                           [&filePath](const std::unique_ptr<Recording> &rec) {
                             return rec->filePath() == filePath;
                           });

  if (it != m_recordings.end()) {
    m_recordings.erase(it, m_recordings.end());
    emit recordingRemoved(filePath);
    updateFilteredRecordings();
  }
}

void RecordingLibrary::updateFilteredRecordings() {
  m_filteredRecordings.clear();

  for (const auto &rec : m_recordings) {
    // Skip active recording
    if (rec->filePath() == m_activeRecordingPath) {
      continue;
    }

    // Apply search filter
    if (!m_searchQuery.isEmpty()) {
      if (!rec->gameName().contains(m_searchQuery, Qt::CaseInsensitive) &&
          !rec->fileName().contains(m_searchQuery, Qt::CaseInsensitive)) {
        continue;
      }
    }

    m_filteredRecordings.append(rec.get());
  }

  // Apply sorting
  switch (m_sortOrder) {
  case NewestFirst:
    std::sort(m_filteredRecordings.begin(), m_filteredRecordings.end(),
              [](Recording *a, Recording *b) { return a->date() > b->date(); });
    break;
  case OldestFirst:
    std::sort(m_filteredRecordings.begin(), m_filteredRecordings.end(),
              [](Recording *a, Recording *b) { return a->date() < b->date(); });
    break;
  case LargestFirst:
    std::sort(m_filteredRecordings.begin(), m_filteredRecordings.end(),
              [](Recording *a, Recording *b) { return a->size() > b->size(); });
    break;
  case NameAZ:
    std::sort(m_filteredRecordings.begin(), m_filteredRecordings.end(),
              [](Recording *a, Recording *b) {
                return a->gameName().compare(b->gameName(),
                                             Qt::CaseInsensitive) < 0;
              });
    break;
  }

  emit recordingsChanged();
}

} // namespace GameRecorder
