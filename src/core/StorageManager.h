/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <vector>

class StorageManagerTest;

namespace TrophyBackend {

/**
 * @brief Manages recording storage and auto-cleanup
 *
 * StorageManager monitors disk usage and automatically deletes
 * oldest recordings when the configured limit is reached.
 */
class StorageManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString storagePath READ storagePath WRITE setStoragePath NOTIFY
                 storagePathChanged)
  Q_PROPERTY(qint64 usedSize READ usedSize NOTIFY usedSizeChanged)
  Q_PROPERTY(qint64 maxSize READ maxSize WRITE setMaxSize NOTIFY maxSizeChanged)
  Q_PROPERTY(double usagePercent READ usagePercent NOTIFY usedSizeChanged)
  Q_PROPERTY(qint64 availableSpace READ availableSpace NOTIFY usedSizeChanged)

  // Total capacity of the drive holding the recordings (upper bound for
  // the limit slider). Re-read when the path changes.
  Q_PROPERTY(qint64 driveCapacityBytes READ driveCapacityBytes NOTIFY
                 storagePathChanged)
  Q_PROPERTY(bool autoDeleteOldest READ autoDeleteOldest WRITE setAutoDeleteOldest NOTIFY autoDeleteOldestChanged)
  // True while a background bulk delete is in flight. Bind UI busy
  // state to this; per-file deleteRecording() stays synchronous.
  Q_PROPERTY(bool bulkDeleteInProgress READ bulkDeleteInProgress NOTIFY
                 bulkDeleteInProgressChanged)

public:
  explicit StorageManager(const QString &storagePath,
                          QObject *parent = nullptr);
  ~StorageManager() override;

  // Path management
  QString storagePath() const { return m_storagePath; }
  void setStoragePath(const QString &path);

  // Size information
  qint64 usedSize() const { return m_usedSize; }
  qint64 maxSize() const { return m_maxSize; }
  void setMaxSize(qint64 bytes);
  bool autoDeleteOldest() const { return m_autoDeleteOldest; }
  void setAutoDeleteOldest(bool enabled);
  void setExcludedRecordingPath(const QString &filePath);
  void clearExcludedRecordingPath();
  double usagePercent() const;
  qint64 availableSpace() const;
  qint64 driveCapacityBytes() const;

  // Operations
  Q_INVOKABLE void refresh();
  // Explicit user-initiated start attempts pass forceNotify=true so every
  // press emits the limit signal even inside an already-notified episode.
  // Background ticks (progress updates, directory watch) leave it false
  // and stay on the once-per-episode latch.
  Q_INVOKABLE bool enforceStorageLimit(bool allowDelete = false,
                                       bool forceNotify = false);
  Q_INVOKABLE bool requestCleanup(bool force = false);
  Q_INVOKABLE bool isOverLimit() const;
  Q_INVOKABLE bool deleteRecording(const QString &filePath);
  // Delete many recordings without blocking the GUI thread. The file
  // I/O runs on the global thread pool; completion arrives as
  // recordingsDeleted(deletedPaths) + a single refresh, then
  // bulkDeleteFinished(deleted, total). Files outside the storage path
  // or already gone are skipped (counted in total, not in deleted).
  // Empty lists are a no-op with no signals.
  Q_INVOKABLE void deleteRecordings(const QStringList &filePaths);
  bool bulkDeleteInProgress() const { return m_bulkDeletesActive > 0; }
  Q_INVOKABLE QString renameRecording(const QString &filePath,
                                      const QString &newTitle);
  Q_INVOKABLE bool exportRecording(const QString &sourcePath, const QString &destDir) const;

  // Formatting helpers
  Q_INVOKABLE static QString formatSize(qint64 bytes);

signals:
  void storagePathChanged();
  void usedSizeChanged();
  void maxSizeChanged();
  void autoDeleteOldestChanged();
  void recordingDeleted(const QString &filePath);
  // Batch counterpart of recordingDeleted, emitted once per
  // deleteRecordings() completion (only when at least one file was
  // actually deleted). Wire to RecordingLibrary::removeRecordings so
  // the model settles once instead of once per file.
  void recordingsDeleted(const QStringList &filePaths);
  void bulkDeleteStarted();
  void bulkDeleteProgress(int deletedCount, int totalCount);
  void bulkDeleteFinished(int deletedCount, int totalCount);
  void bulkDeleteInProgressChanged();
  void recordingRenamed(const QString &oldPath, const QString &newPath);
  void storageLimitReached();
  void storageLimitCannotCleanup();

private slots:
  void onDirectoryChanged(const QString &path);

private:
  friend class ::StorageManagerTest;
  // Test-only observation of real refresh passes, including unchanged-size
  // scans which cannot be counted through usedSizeChanged.
  std::function<void()> m_refreshObserverForTest;
  qint64 calculateDirectorySize() const;
  QStringList getRecordingsSortedByDate() const;
  bool deleteOldestRecording();
  // Single-walk snapshot for enforceStorageLimit: one recursive walk per
  // pass instead of one per deleted file. Sorted oldest-first, matching
  // getRecordingsSortedByDate() order.
  struct SnapshotEntry {
    QString path;
    QDateTime mtime;
    qint64 size = 0;
  };
  std::vector<SnapshotEntry> snapshotRecordings() const;
  // Same checks/thumb cleanup/recordingDeleted emit as deleteRecording(),
  // but without the trailing refresh() so enforceStorageLimit can settle
  // once after deleting many files.
  bool deleteRecordingNoRefresh(const QString &filePath);
  // GUI-thread completion for deleteRecordings(): batch signal, single
  // refresh, progress close-out.
  void finishBulkDelete(const QStringList &deletedPaths, int totalCount);

  QString m_storagePath;
  qint64 m_usedSize = 0;
  qint64 m_maxSize = 50LL * 1024 * 1024 * 1024; // 50 GB default
  bool m_autoDeleteOldest = false;
  QString m_excludedRecordingPath;
  // Latch so the limit notices fire once per over-limit episode instead
  // of on every progress tick / directory change while over. Cleared
  // whenever usage drops back under the limit or the limit settings
  // change (a new situation is worth one fresh notice).
  bool m_limitNotified = false;
  void notifyLimitReached(bool force = false);
  void notifyLimitCannotCleanup(bool force = false);

  // In-flight background bulk deletes. While > 0 the directory
  // watcher is suppressed (a bulk unlink storm would otherwise
  // re-enter enforceStorageLimit per file on a stale size); the bulk
  // completion settles with a single refresh instead.
  int m_bulkDeletesActive = 0;
  QFileSystemWatcher m_watcher;
  // Coalesces filesystem-event bursts (a 50-file import fires per file)
  // into a single enforceStorageLimit pass (see onDirectoryChanged).
  QTimer m_debounceTimer;
};

} // namespace TrophyBackend
