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
#include <QFile>
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
    refresh();
    if (isOverLimit()) {
      if (m_autoDeleteOldest) {
        emit storageLimitReached();
      } else {
        emit storageLimitCannotCleanup();
      }
    }
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

qint64 StorageManager::driveCapacityBytes() const {
  QStorageInfo storage(m_storagePath);
  const qint64 total = storage.bytesTotal();
  return total > 0 ? total : 0;
}

void StorageManager::refresh() {
  qint64 newSize = calculateDirectorySize();
  if (m_usedSize != newSize) {
    m_usedSize = newSize;
    emit usedSizeChanged();
  }
}

void StorageManager::setAutoDeleteOldest(bool enabled) {
  if (m_autoDeleteOldest != enabled) {
    m_autoDeleteOldest = enabled;
    emit autoDeleteOldestChanged();
    refresh();
    if (isOverLimit()) {
      if (m_autoDeleteOldest) {
        emit storageLimitReached();
      } else {
        emit storageLimitCannotCleanup();
      }
    }
  }
}

void StorageManager::setExcludedRecordingPath(const QString &filePath) {
  QFileInfo info(filePath);
  m_excludedRecordingPath = info.canonicalFilePath();
  if (m_excludedRecordingPath.isEmpty()) {
    m_excludedRecordingPath = info.absoluteFilePath();
  }
}

void StorageManager::clearExcludedRecordingPath() {
  m_excludedRecordingPath.clear();
}

bool StorageManager::requestCleanup(bool force) {
  if (m_maxSize <= 0) {
    return false;
  }

  if (!m_autoDeleteOldest && !force) {
    return false;
  }

  return enforceStorageLimit(force);
}

bool StorageManager::isOverLimit() const {
  return m_maxSize > 0 && m_usedSize > m_maxSize;
}

bool StorageManager::enforceStorageLimit(bool allowDelete) {
  refresh();

  if (m_maxSize <= 0) {
    // A non-positive max size means "no limit"; do not delete recordings.
    return true;
  }

  if (!m_autoDeleteOldest && !allowDelete) {
    if (m_usedSize > m_maxSize) {
      emit storageLimitCannotCleanup();
      return false;
    }
    return true;
  }

  while (m_usedSize > m_maxSize) {
    qDebug() << "Storage limit exceeded:" << formatSize(m_usedSize) << "/"
            << formatSize(m_maxSize);

    if (!deleteOldestRecording()) {
      // If we can't delete the oldest, check if it's because it's the current recording
      QStringList recordings = getRecordingsSortedByDate();
      if (!recordings.isEmpty() && recordings.first() == m_excludedRecordingPath) {
        emit storageLimitCannotCleanup();
      }
      break;
    }

    refresh();
  }

  if (m_usedSize > m_maxSize * 0.9) {
    emit storageLimitReached();
  }

  return !isOverLimit();
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
    // Remove the sidecar thumbnail too, so auto-cleanup doesn't orphan
    // .thumbnails/<basename>.jpg. Kept idempotent with
    // RecordingLibrary::removeRecording (which does the same when the
    // recordingDeleted signal is wired up in Application).
    const QString thumbPath = info.absolutePath() + "/.thumbnails/" +
                              info.completeBaseName() + ".jpg";
    if (QFile::exists(thumbPath) && !QFile::remove(thumbPath)) {
      qWarning() << "Failed to delete thumbnail:" << thumbPath;
    }
    emit recordingDeleted(filePath);
    refresh();
    return true;
  }

  qWarning() << "Failed to delete recording:" << filePath << file.errorString();
  return false;
}

bool StorageManager::deleteOldestRecording() {
  const QStringList recordings = getRecordingsSortedByDate();

  if (recordings.isEmpty()) {
    qWarning() << "No recordings to delete";
    return false;
  }

  const QString excludedCanonical =
      m_excludedRecordingPath.isEmpty()
          ? QString()
          : QFileInfo(m_excludedRecordingPath).canonicalFilePath();

  // Delete the oldest recording that is not the in-progress (excluded)
  // one. Giving up at the first excluded entry would disable auto-cleanup
  // entirely whenever the active recording happens to be the oldest file.
  for (const QString &candidate : recordings) {
    if (!excludedCanonical.isEmpty() &&
        QFileInfo(candidate).canonicalFilePath() == excludedCanonical) {
      qDebug() << "Not deleting current recording:" << candidate;
      continue;
    }
    qDebug() << "Auto-deleting oldest recording:" << candidate;
    return deleteRecording(candidate);
  }

  qDebug() << "Only the current recording remains; nothing to delete";
  return false;
}

QString StorageManager::renameRecording(const QString &filePath,
                                        const QString &newTitle) {
  QFileInfo info(filePath);
  const QString oldAbsolute = info.absoluteFilePath();

  // Security check: ensure file is within storage path
  if (!oldAbsolute.startsWith(m_storagePath)) {
    qWarning() << "Refusing to rename file outside storage path:" << filePath;
    return QString();
  }

  if (!info.exists() || !info.isFile()) {
    qWarning() << "Cannot rename, file does not exist:" << filePath;
    return QString();
  }

  // Never rename the in-progress recording: the muxer is still writing to it.
  if (!m_excludedRecordingPath.isEmpty()) {
    const QString excludedCanonical =
        QFileInfo(m_excludedRecordingPath).canonicalFilePath();
    const QString candidateCanonical = info.canonicalFilePath();
    if (!excludedCanonical.isEmpty() &&
        candidateCanonical == excludedCanonical) {
      qWarning() << "Refusing to rename in-progress recording:" << filePath;
      return QString();
    }
  }

  // Sanitize the user-supplied title the same way new recording filenames
  // are sanitized, so renamed files still parse for game name / timestamp.
  QString safeName = newTitle.trimmed();
  safeName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
  safeName.replace(QRegularExpression("[_-]{2,}"), "_");
  safeName =
      safeName.trimmed().replace(QRegularExpression("^[_-]+|[_-]+$"), "");
  if (safeName.isEmpty()) {
    qWarning() << "Refusing to rename: title is empty after sanitizing:"
               << newTitle;
    return QString();
  }

  // Keep the timestamp suffix so the recording date stays visible in the
  // filename and parseFileNameForGameName keeps working.
  const QString oldBase = info.completeBaseName();
  static const QRegularExpression timestampRe(
      "_\\d{4}-\\d{2}-\\d{2}_\\d{2}-\\d{2}-\\d{2}$");
  QString newBase = safeName;
  QRegularExpressionMatch match = timestampRe.match(oldBase);
  if (match.hasMatch()) {
    newBase += match.captured(0);
  }

  const QString suffix = info.suffix();
  QDir dir(info.absolutePath());
  QString newAbsolute =
      dir.absoluteFilePath(newBase + (suffix.isEmpty() ? QString() : "." + suffix));

  if (newAbsolute == oldAbsolute) {
    return oldAbsolute; // No-op: title already matches.
  }

  // Collision avoidance: same _1/_2 suffix scheme as exportRecording.
  if (QFile::exists(newAbsolute)) {
    int counter = 1;
    while (QFile::exists(newAbsolute)) {
      newAbsolute = dir.absoluteFilePath(
          QString("%1_%2.%3").arg(newBase).arg(counter).arg(suffix));
      counter++;
    }
  }

  if (!QFile::rename(oldAbsolute, newAbsolute)) {
    qWarning() << "Failed to rename recording:" << oldAbsolute << "->"
               << newAbsolute;
    return QString();
  }

  // Move the sidecar thumbnail alongside the video so the library picks it
  // up instead of regenerating it.
  const QString thumbDir = info.absolutePath() + "/.thumbnails";
  const QString oldThumb = thumbDir + "/" + oldBase + ".jpg";
  if (QFile::exists(oldThumb)) {
    const QString newThumb =
        thumbDir + "/" + QFileInfo(newAbsolute).completeBaseName() + ".jpg";
    if (!QFile::exists(newThumb)) {
      QFile::rename(oldThumb, newThumb);
    } else {
      QFile::remove(oldThumb);
    }
  }

  qDebug() << "Renamed recording:" << oldAbsolute << "->" << newAbsolute;
  emit recordingRenamed(oldAbsolute, newAbsolute);
  refresh();
  return newAbsolute;
}

bool StorageManager::exportRecording(const QString &sourcePath, const QString &destDir) const {
  QFileInfo sourceInfo(sourcePath);
  if (!sourceInfo.exists()) {
    qWarning() << "Export source does not exist:" << sourcePath;
    return false;
  }

  QDir dir(destDir);
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      qWarning() << "Failed to create export directory:" << destDir;
      return false;
    }
  }

  QString destPath = dir.filePath(sourceInfo.fileName());
  
  // If file already exists, don't overwrite blindly, maybe add a suffix
  if (QFile::exists(destPath)) {
    QString baseName = sourceInfo.completeBaseName();
    QString ext = sourceInfo.suffix();
    int counter = 1;
    while (QFile::exists(destPath)) {
      destPath = dir.filePath(QString("%1_%2.%3").arg(baseName).arg(counter).arg(ext));
      counter++;
    }
  }

  if (QFile::copy(sourcePath, destPath)) {
    qDebug() << "Exported recording to:" << destPath;
    return true;
  }

  qWarning() << "Failed to export recording to:" << destPath;
  return false;
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
  enforceStorageLimit();
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

} // namespace TrophyBackend
