/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

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
  Q_INVOKABLE bool enforceStorageLimit(bool allowDelete = false);
  Q_INVOKABLE bool requestCleanup(bool force = false);
  Q_INVOKABLE bool isOverLimit() const;
  Q_INVOKABLE bool deleteRecording(const QString &filePath);
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
  void recordingRenamed(const QString &oldPath, const QString &newPath);
  void storageLimitReached();
  void storageLimitCannotCleanup();

private slots:
  void onDirectoryChanged(const QString &path);

private:
  qint64 calculateDirectorySize() const;
  QStringList getRecordingsSortedByDate() const;
  bool deleteOldestRecording();

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
  void notifyLimitReached();
  void notifyLimitCannotCleanup();

  QFileSystemWatcher m_watcher;
};

} // namespace TrophyBackend
