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
  double usagePercent() const;
  qint64 availableSpace() const;

  // Operations
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void enforceStorageLimit();
  Q_INVOKABLE bool deleteRecording(const QString &filePath);
  Q_INVOKABLE QString generateFilePath(const QString &baseName,
                                       const QString &extension) const;
  Q_INVOKABLE bool exportRecording(const QString &sourcePath, const QString &destDir) const;

  // Formatting helpers
  Q_INVOKABLE static QString formatSize(qint64 bytes);

signals:
  void storagePathChanged();
  void usedSizeChanged();
  void maxSizeChanged();
  void recordingDeleted(const QString &filePath);
  void storageLimitReached();

private slots:
  void onDirectoryChanged(const QString &path);

private:
  qint64 calculateDirectorySize() const;
  QStringList getRecordingsSortedByDate() const;
  void deleteOldestRecording();

  QString m_storagePath;
  qint64 m_usedSize = 0;
  qint64 m_maxSize = 50LL * 1024 * 1024 * 1024; // 50 GB default

  QFileSystemWatcher m_watcher;
};

} // namespace TrophyBackend
