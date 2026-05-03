/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "StorageManager.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>

namespace TrophyBackend {

StorageManager::StorageManager(const QString &storagePath, QObject *parent)
    : QObject(parent), m_storagePath(storagePath) {
  // Ensure storage directory exists
  QDir dir(m_storagePath);
  if (!dir.exists()) {
    dir.mkpath(".");
  }

  // Watch for changes
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &StorageManager::onDirectoryChanged);

  if (dir.exists()) {
    m_watcher.addPath(m_storagePath);
  }

  // Initial size calculation
  refresh();
}

StorageManager::~StorageManager() = default;

void StorageManager::setStoragePath(const QString &path) {
  if (m_storagePath != path) {
    // Remove old watch
    if (!m_storagePath.isEmpty()) {
      m_watcher.removePath(m_storagePath);
    }

    m_storagePath = path;

    // Ensure new directory exists
    QDir dir(m_storagePath);
    if (!dir.exists()) {
      dir.mkpath(".");
    }

    // Add new watch
    m_watcher.addPath(m_storagePath);

    emit storagePathChanged();
    refresh();
  }
}

void StorageManager::setMaxSize(qint64 bytes) {
  if (m_maxSize != bytes) {
    m_maxSize = bytes;
    emit maxSizeChanged();
    enforceStorageLimit();
  }
}

double StorageManager::usagePercent() const {
  if (m_maxSize <= 0) {
    return 0.0;
  }
  return static_cast<double>(m_usedSize) / static_cast<double>(m_maxSize) *
         100.0;
}

qint64 StorageManager::availableSpace() const {
  QStorageInfo storage(m_storagePath);
  return storage.bytesAvailable();
}

void StorageManager::refresh() {
  qint64 newSize = calculateDirectorySize();
  if (m_usedSize != newSize) {
    m_usedSize = newSize;
    emit usedSizeChanged();
  }
}

void StorageManager::enforceStorageLimit() {
  refresh();

  while (m_usedSize > m_maxSize) {
    qDebug() << "Storage limit exceeded:" << formatSize(m_usedSize) << "/"
            << formatSize(m_maxSize);

    deleteOldestRecording();

    // Recalculate after deletion
    qint64 newSize = calculateDirectorySize();
    if (newSize >= m_usedSize) {
      // Nothing more to delete or size didn't change
      break;
    }
    m_usedSize = newSize;
  }

  if (m_usedSize > m_maxSize * 0.9) {
    emit storageLimitReached();
  }
}

bool StorageManager::deleteRecording(const QString &filePath) {
  QFileInfo info(filePath);

  // Security check: ensure file is within storage path
  if (!info.absoluteFilePath().startsWith(m_storagePath)) {
    qWarning() << "Refusing to delete file outside storage path:" << filePath;
    return false;
  }

  if (!info.exists()) {
    qWarning() << "File does not exist:" << filePath;
    return false;
  }

  QFile file(filePath);
  if (file.remove()) {
    qDebug() << "Deleted recording:" << filePath;
    emit recordingDeleted(filePath);
    refresh();
    return true;
  }

  qWarning() << "Failed to delete recording:" << filePath << file.errorString();
  return false;
}

QString StorageManager::generateFilePath(const QString &baseName,
                                         const QString &extension) const {
  QString safeName = baseName;
  safeName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");

  QString timestamp =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
  QString fileName = QString("%1_%2.%3").arg(safeName, timestamp, extension);

  return m_storagePath + "/" + fileName;
}

QString StorageManager::formatSize(qint64 bytes) {
  const qint64 KB = 1024;
  const qint64 MB = KB * 1024;
  const qint64 GB = MB * 1024;
  const qint64 TB = GB * 1024;

  if (bytes >= TB) {
    return QString("%1 TB").arg(static_cast<double>(bytes) / TB, 0, 'f', 2);
  } else if (bytes >= GB) {
    return QString("%1 GB").arg(static_cast<double>(bytes) / GB, 0, 'f', 2);
  } else if (bytes >= MB) {
    return QString("%1 MB").arg(static_cast<double>(bytes) / MB, 0, 'f', 1);
  } else if (bytes >= KB) {
    return QString("%1 KB").arg(static_cast<double>(bytes) / KB, 0, 'f', 1);
  }
  return QString("%1 B").arg(bytes);
}

void StorageManager::onDirectoryChanged(const QString &path) {
  Q_UNUSED(path)
  refresh();
}

qint64 StorageManager::calculateDirectorySize() const {
  qint64 totalSize = 0;

  QDirIterator it(m_storagePath,
                  QStringList() << "*.mkv" << "*.mp4" << "*.webm", QDir::Files,
                  QDirIterator::Subdirectories);

  while (it.hasNext()) {
    it.next();
    totalSize += it.fileInfo().size();
  }

  return totalSize;
}

QStringList StorageManager::getRecordingsSortedByDate() const {
  QDir dir(m_storagePath);
  QFileInfoList files = dir.entryInfoList(
      QStringList() << "*.mkv" << "*.mp4" << "*.webm", QDir::Files,
      QDir::Time | QDir::Reversed // Oldest first
  );

  QStringList paths;
  for (const QFileInfo &info : files) {
    paths.append(info.absoluteFilePath());
  }

  return paths;
}

void StorageManager::deleteOldestRecording() {
  QStringList recordings = getRecordingsSortedByDate();

  if (recordings.isEmpty()) {
    qWarning() << "No recordings to delete";
    return;
  }

  QString oldest = recordings.first();
  qDebug() << "Auto-deleting oldest recording:" << oldest;
  deleteRecording(oldest);
}

} // namespace TrophyBackend
