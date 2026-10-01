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
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>

namespace TrophyBackend {

namespace {
constexpr char kCustomTitlesFile[] = ".trophy-titles.json";
constexpr char kCustomTitlesKey[] = "preferFileNameTitle";
} // namespace

RecordingLibrary::RecordingLibrary(const QString &libraryPath, QObject *parent)
    : QObject(parent), m_libraryPath(libraryPath),
      m_thumbnailGenerator(std::make_unique<ThumbnailGenerator>(this)) {
  // Connect thumbnail generator
  connect(m_thumbnailGenerator.get(), &ThumbnailGenerator::thumbnailReady, this,
          &RecordingLibrary::onThumbnailReady);

  // Watch for changes
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &RecordingLibrary::onDirectoryChanged);

  loadCustomTitles();

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
    loadCustomTitles();
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
  scanDirectory();
  updateFilteredRecordings();
}

Recording *RecordingLibrary::get(int index) const {
  if (index < 0 || index >= m_filteredRecordings.size()) {
    return nullptr;
  }
  return m_filteredRecordings[index];
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
  m_ignoreNextChange = true;
  QFile file(filePath);
  if (file.remove()) {
    removeRecording(filePath);
  } else {
    m_ignoreNextChange = false;
    qWarning() << "Failed to delete recording:" << filePath
               << file.errorString();
  }
}

void RecordingLibrary::setActiveRecordingPath(const QString &path) {
  m_activeRecordingPath = path;
  updateFilteredRecordings();
}

void RecordingLibrary::onRecordingRenamed(const QString &oldPath,
                                           const QString &newPath) {
  // Every in-app rename makes the new filename authoritative: the file
  // still carries the pre-rename container title tag.
  m_fileNameTitlePreferred.remove(QFileInfo(oldPath).fileName());
  if (!newPath.isEmpty()) {
    m_fileNameTitlePreferred.insert(QFileInfo(newPath).fileName());
    // Mark the source so the refresh below doesn't report its
    // disappearance as a deletion (see removeRecording).
    m_renameSource = oldPath;
  }
  saveCustomTitles();
}

void RecordingLibrary::onDirectoryChanged(const QString &path) {
  Q_UNUSED(path)
  if (m_ignoreNextChange) {
    m_ignoreNextChange = false;
    return;
  }
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
    qDebug() << "Library path does not exist:" << m_libraryPath;
    if (!m_recordings.empty()) {
      m_recordings.clear();
      updateFilteredRecordings();
    }
    return;
  }

  // Add directory watch
  if (!m_watcher.directories().contains(m_libraryPath)) {
    m_watcher.addPath(m_libraryPath);
  }

  // Scan for video files
  QStringList filters = {"*.mkv", "*.mp4", "*.webm"};
  QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

  // Identify files to remove
  QStringList currentFiles;
  for (const QFileInfo &fileInfo : files) {
    currentFiles << fileInfo.absoluteFilePath();
  }

  std::vector<QString> toRemove;
  for (const auto &rec : m_recordings) {
    if (!currentFiles.contains(rec->filePath())) {
      toRemove.push_back(rec->filePath());
    }
  }

  for (const auto &path : toRemove) {
    removeRecording(path);
  }

  // Add new files
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
  recording->setPreferFileNameTitle(
      m_fileNameTitlePreferred.contains(QFileInfo(filePath).fileName()));
  recording->loadMetadata();

  // Request thumbnail generation
  if (recording->thumbnailPath().isEmpty()) {
    m_thumbnailGenerator->generateThumbnail(filePath);
  }

  m_recordings.push_back(std::move(recording));

  // Settle the filtered list before notifying: observers calling get()
  // inside the slot (the dashboard activity feed does) must never see a
  // stale list.
  updateFilteredRecordings();
  emit recordingAdded(m_recordings.back().get());
}

void RecordingLibrary::removeRecording(const QString &filePath) {
  // Drop any rename preference for the deleted file.
  if (m_fileNameTitlePreferred.remove(QFileInfo(filePath).fileName())) {
    saveCustomTitles();
  }

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
    // Settle the filtered list BEFORE emitting: it still holds the raw
    // pointer that erase() just freed, and any observer calling get()
    // inside the slot would wrap a dangling QObject (live SIGSEGV,
    // 2026-10-01: deleting a library item crashed in QV4::fromData via
    // the new activity-feed onRecordingRemoved handler).
    updateFilteredRecordings();
    // A rename's source path vanishes from disk too, but that is not a
    // deletion — the renamed file arrives in the same refresh. Swallow
    // the signal (marked by onRecordingRenamed) so renames don't show
    // up as deletions in the activity feed.
    if (filePath == m_renameSource) {
      m_renameSource.clear();
    } else {
      emit recordingRemoved(filePath);
    }
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

QString RecordingLibrary::customTitlesPath() const {
  return QDir(m_libraryPath).absoluteFilePath(kCustomTitlesFile);
}

void RecordingLibrary::loadCustomTitles() {
  m_fileNameTitlePreferred.clear();

  QFile file(customTitlesPath());
  if (!file.exists()) {
    return;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "Failed to read custom titles:" << file.fileName();
    return;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject()) {
    qWarning() << "Ignoring malformed custom titles:" << file.fileName();
    return;
  }
  const QJsonArray names =
      doc.object().value(kCustomTitlesKey).toArray();
  for (const QJsonValue &v : names) {
    if (v.isString() && !v.toString().isEmpty()) {
      m_fileNameTitlePreferred.insert(v.toString());
    }
  }
}

void RecordingLibrary::saveCustomTitles() const {
  if (m_libraryPath.isEmpty()) {
    return;
  }
  QDir dir(m_libraryPath);
  if (!dir.exists() && !dir.mkpath(".")) {
    qWarning() << "Failed to create library path for custom titles:"
               << m_libraryPath;
    return;
  }
  QJsonArray names;
  for (const QString &name : m_fileNameTitlePreferred) {
    names.append(name);
  }
  QJsonObject root;
  root.insert(kCustomTitlesKey, names);

  QSaveFile file(customTitlesPath());
  if (!file.open(QIODevice::WriteOnly)) {
    qWarning() << "Failed to write custom titles:" << file.fileName();
    return;
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  if (!file.commit()) {
    qWarning() << "Failed to commit custom titles:" << file.fileName();
  }
}

} // namespace TrophyBackend
