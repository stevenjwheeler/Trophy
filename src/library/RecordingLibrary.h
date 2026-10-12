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
#include <QUrl>
#include <QString>
#include <QStringList>
#include <memory>
#include <thread>
#include <vector>

namespace TrophyBackend {

class Recording;
class ThumbnailGenerator;
class StorageManager;

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
                 libraryPathChanged)  Q_PROPERTY(QString searchQuery READ searchQuery WRITE setSearchQuery NOTIFY
                 recordingsChanged)
  Q_PROPERTY(
      int sortOrder READ sortOrder WRITE setSortOrder NOTIFY recordingsChanged)
  // True while a player-range clip is remuxing on the worker thread.
  Q_PROPERTY(bool clipSaving READ clipSaving NOTIFY clipSavingChanged)

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

  bool clipSaving() const { return m_clipSaving; }

  Q_INVOKABLE void refresh();
  Q_INVOKABLE Recording *get(int index) const;
  Q_INVOKABLE Recording *getRecording(const QString &filePath) const;
  Q_INVOKABLE QUrl localFileUrl(const QString &filePath) const;
  Q_INVOKABLE void deleteRecording(const QString &filePath);
  Q_INVOKABLE void removeRecording(const QString &filePath);
  /**
   * @brief Cuts [startSeconds, endSeconds] out of a library file into a
   * new clip alongside it (same naming/tags as DVR clips).
   *
   * Async: the remux runs on a worker thread (.part + atomic rename, so
   * the watcher never sees a half-written file) and reports back via
   * clipSaved/clipSaveFailed. While one runs, further calls fail fast
   * with clipSaveFailed. Guards mirror ReplayClipper::saveRange.
   */
  Q_INVOKABLE void saveClipRange(const QString &sourcePath,
                                 double startSeconds, double endSeconds);
  // Optional storage gate for saveClipRange (see ReplayManager::m_storage
  // for the policy). Wired by Application; unset means no limit checks.
  void setStorageManager(StorageManager *storage) { m_storage = storage; }
  // Batch counterpart of removeRecording: erases every listed path in
  // one pass and settles the filtered model once, then emits
  // recordingsRemoved with the paths actually dropped. Emits nothing
  // when none of the paths were tracked. Direct calls are unaffected
  // by setBulkOperationActive — only watcher refreshes are gated.
  Q_INVOKABLE void removeRecordings(const QStringList &filePaths);

 public slots:
  // Suppresses watcher-driven refresh() while a background bulk
  // delete is unlinking files (each vanished file would otherwise
  // settle + notify on its own). The bulk completion path calls the
  // batch removal above and refreshes explicitly.
  void setBulkOperationActive(bool active);

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
  // Batch counterpart of recordingRemoved, emitted once per
  // removeRecordings() with every path actually dropped from the
  // model. Prefer this over N per-file emissions for bulk flows: each
  // per-file emit fans out to StorageManager::refresh (a full size
  // rescan) and a QML model rebuild.
  void recordingsRemoved(const QStringList &filePaths);
  void clipSavingChanged();
  // Player-range clip finished: path of the new clip, the game name it
  // was filed under, and the span the file actually holds (keyframe
  // overshoot runs up to a GOP over the selection).
  void clipSaved(const QString &filePath, const QString &gameName,
                 double seconds);
  void clipSaveFailed(const QString &reason);

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
  // Optional storage gate for saveClipRange (plain pointer: the manager
  // is app-owned and outlives the library; never dereferenced off-thread).
  StorageManager *m_storage = nullptr;
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
  // Depth of active background bulk deletes (see setBulkOperationActive).
  // Counter, not bool, so overlapping bulks can't unsuppress each other.
  int m_bulkOperationDepth = 0;
  // A player-range clip is remuxing (see saveClipRange). Counter like
  // the bulk depth is overkill — QML gates on the flag and re-entrant
  // calls fail fast — but the flag itself must only flip on the GUI
  // thread (worker completion hops back via queued invoke).
  bool m_clipSaving = false;
  std::thread m_clipThread;
};

} // namespace TrophyBackend
