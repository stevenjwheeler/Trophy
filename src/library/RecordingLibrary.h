/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFileSystemWatcher>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <memory>
#include <vector>

namespace TrophyBackend {

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
  Q_INVOKABLE Recording *get(int index) const;
  Q_INVOKABLE Recording *getRecording(const QString &filePath) const;
  Q_INVOKABLE void deleteRecording(const QString &filePath);
  Q_INVOKABLE void removeRecording(const QString &filePath);

 public slots:
  /**
   * @brief Record a rename so the new filename stays authoritative.
   *
   * Connect to StorageManager::recordingRenamed BEFORE refresh: the
   * container still carries the pre-rename title tag, so without this the
   * background probe would revert the displayed name to the stale tag.
   */
  void onRecordingRenamed(const QString &oldPath, const QString &newPath);

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
  void loadCustomTitles();
  void saveCustomTitles() const;
  QString customTitlesPath() const;

  QString m_libraryPath;
  QString m_searchQuery;
  int m_sortOrder = NewestFirst;
  QString m_activeRecordingPath;
  std::vector<std::unique_ptr<Recording>> m_recordings;
  QList<Recording *> m_filteredRecordings;
  QFileSystemWatcher m_watcher;
  std::unique_ptr<ThumbnailGenerator> m_thumbnailGenerator;
  bool m_ignoreNextChange = false;
  // Source path of an in-flight user rename (set by onRecordingRenamed,
  // consumed by the refresh that follows). The old file is genuinely
  // gone from disk, but observers must not hear recordingRemoved for
  // it — otherwise renames show up as deletions in the activity feed.
  QString m_renameSource;
  // File names (not paths) whose filename-derived title wins over the
  // container title tag. Populated by user renames, persisted next to the
  // library so it survives restarts and library moves.
  QSet<QString> m_fileNameTitlePreferred;
};

} // namespace TrophyBackend
