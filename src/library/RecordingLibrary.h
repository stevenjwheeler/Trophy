/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

namespace GameRecorder {

class Recording;
class ThumbnailGenerator;

/**
 * @brief Manages the library of recorded videos
 *
 * Scans the recordings directory, maintains metadata about each recording,
 * and generates thumbnails.
 */
class RecordingLibrary : public QObject {
  Q_OBJECT

  Q_PROPERTY(
      QList<Recording *> recordings READ recordings NOTIFY recordingsChanged)
  Q_PROPERTY(int count READ count NOTIFY recordingsChanged)
  Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY
                 libraryPathChanged)
  Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY
                 recordingsChanged)
  Q_PROPERTY(
      int sortOrder READ sortOrder WRITE setSortOrder NOTIFY recordingsChanged)

public:
  enum SortOrder { NewestFirst = 0, OldestFirst, LargestFirst, NameAZ };
  Q_ENUM(SortOrder)

  explicit RecordingLibrary(const QString &libraryPath,
                            QObject *parent = nullptr);
  ~RecordingLibrary() override;

  QList<Recording *> recordings() const;
  int count() const;

  QString libraryPath() const { return m_libraryPath; }
  void setLibraryPath(const QString &path);

  QString searchQuery() const { return m_searchQuery; }
  void setSearchQuery(const QString &query);

  int sortOrder() const { return m_sortOrder; }
  void setSortOrder(int order);

  Q_INVOKABLE void refresh();
  Q_INVOKABLE Recording *getRecording(const QString &filePath) const;
  Q_INVOKABLE void deleteRecording(const QString &filePath);
  Q_INVOKABLE void removeRecording(const QString &filePath);

  void setActiveRecordingPath(const QString &path);

signals:
  void recordingsChanged();
  void libraryPathChanged();
  void recordingAdded(Recording *recording);
  void recordingRemoved(const QString &filePath);

private slots:
  void onDirectoryChanged(const QString &path);
  void onThumbnailReady(const QString &videoPath, const QString &thumbnailPath);

private:
  void scanDirectory();
  void addRecording(const QString &filePath);
  void updateFilteredRecordings();

  QString m_libraryPath;
  QString m_searchQuery;
  int m_sortOrder = NewestFirst;
  QString m_activeRecordingPath;
  std::vector<std::unique_ptr<Recording>> m_recordings;
  QList<Recording *> m_filteredRecordings;
  QFileSystemWatcher m_watcher;
  std::unique_ptr<ThumbnailGenerator> m_thumbnailGenerator;
};

} // namespace GameRecorder
