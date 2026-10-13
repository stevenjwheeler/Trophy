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
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QThreadPool>
#include <algorithm>

namespace TrophyBackend {

namespace {

// Require both the supplied path and its resolved target to stay under the
// configured library. The lexical check rejects sibling-prefix paths (for
// example, /videos-old when the library is /videos); the canonical check
// also rejects symlinks that escape the library.
bool isWithinStoragePath(const QString &filePath, const QString &storagePath) {
  const QFileInfo rootInfo(storagePath);
  const QString lexicalRoot = QDir::cleanPath(rootInfo.absoluteFilePath());
  const QString resolvedRoot = rootInfo.canonicalFilePath();
  const QFileInfo candidateInfo(filePath);
  const QString candidate = QDir::cleanPath(candidateInfo.absoluteFilePath());
  const QString resolvedCandidate = candidateInfo.canonicalFilePath();
  if (lexicalRoot.isEmpty() || resolvedRoot.isEmpty() ||
      resolvedCandidate.isEmpty())
    return false;

  auto isChildOf = [](const QString &path, const QString &root) {
    if (path == root)
      return true;
    QString prefix = root;
    if (!prefix.endsWith(QDir::separator()))
      prefix += QDir::separator();
    return path.startsWith(prefix);
  };

  return candidate != lexicalRoot && isChildOf(candidate, lexicalRoot) &&
         isChildOf(resolvedCandidate, resolvedRoot);
}

} // namespace

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

  // Filesystem events arrive per file (a bulk import is a storm of
  // them); coalesce each burst into one enforceStorageLimit pass.
  m_debounceTimer.setSingleShot(true);
  m_debounceTimer.setInterval(500);
  connect(&m_debounceTimer, &QTimer::timeout, this, [this]() {
    if (m_bulkDeletesActive > 0) {
      return;
    }
    enforceStorageLimit();
  });

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
    m_limitNotified = false;
    if (isOverLimit()) {
      if (m_autoDeleteOldest) {
        notifyLimitReached();
      } else {
        notifyLimitCannotCleanup();
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
  if (m_refreshObserverForTest) {
    m_refreshObserverForTest();
  }
  qint64 newSize = calculateDirectorySize();
  if (m_usedSize != newSize) {
    m_usedSize = newSize;
    emit usedSizeChanged();
  }
  if (!isOverLimit()) {
    m_limitNotified = false;
  }
}

void StorageManager::setAutoDeleteOldest(bool enabled) {
  if (m_autoDeleteOldest != enabled) {
    m_autoDeleteOldest = enabled;
    emit autoDeleteOldestChanged();
    refresh();
    m_limitNotified = false;
    if (isOverLimit()) {
      if (m_autoDeleteOldest) {
        notifyLimitReached();
      } else {
        notifyLimitCannotCleanup();
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

void StorageManager::notifyLimitReached(bool force) {
  if (m_limitNotified && !force) {
    return;
  }
  m_limitNotified = true;
  emit storageLimitReached();
}

void StorageManager::notifyLimitCannotCleanup(bool force) {
  if (m_limitNotified && !force) {
    return;
  }
  m_limitNotified = true;
  emit storageLimitCannotCleanup();
}

bool StorageManager::enforceStorageLimit(bool allowDelete, bool forceNotify) {
  refresh();

  if (m_maxSize <= 0) {
    // A non-positive max size means "no limit"; do not delete recordings.
    return true;
  }

  if (!m_autoDeleteOldest && !allowDelete) {
    if (m_usedSize > m_maxSize) {
      notifyLimitCannotCleanup(forceNotify);
      return false;
    }
    return true;
  }

  if (m_usedSize <= m_maxSize) {
    return true;
  }

  // Snapshot the library once and delete oldest-first from it: the old
  // loop re-ran getRecordingsSortedByDate() + refresh() (each a full
  // recursive walk) per deleted file. Deletion order and in-progress
  // exclusion match deleteOldestRecording(); only the file being deleted
  // is re-statted, guarding against a vanish/change since the snapshot.
  const std::vector<SnapshotEntry> snapshot = snapshotRecordings();
  const QString excludedCanonical =
      m_excludedRecordingPath.isEmpty()
          ? QString()
          : QFileInfo(m_excludedRecordingPath).canonicalFilePath();
  auto isExcluded = [&excludedCanonical](const QString &path) {
    return !excludedCanonical.isEmpty() &&
           QFileInfo(path).canonicalFilePath() == excludedCanonical;
  };

  qint64 runningSize = m_usedSize;
  QSet<QString> deletedPaths;
  bool deleteFailed = false;
  for (const SnapshotEntry &entry : snapshot) {
    if (runningSize <= m_maxSize) {
      break;
    }
    if (isExcluded(entry.path)) {
      qDebug() << "Not deleting current recording:" << entry.path;
      continue;
    }
    const QFileInfo fresh(entry.path);
    if (!fresh.exists()) {
      continue;
    }
    qDebug() << "Storage limit exceeded:" << formatSize(runningSize) << "/"
             << formatSize(m_maxSize);
    qDebug() << "Auto-deleting oldest recording:" << entry.path;
    if (!deleteRecordingNoRefresh(entry.path)) {
      deleteFailed = true;
      break;
    }
    deletedPaths.insert(entry.path);
    runningSize -= fresh.size();
  }

  // One settle walk for the whole pass instead of one per deleted file.
  // Skipped when nothing was deleted: m_usedSize is already current from
  // the entry refresh.
  if (!deletedPaths.isEmpty()) {
    refresh();
  }

  if (isOverLimit()) {
    // Mirror the old deleteOldestRecording-failure branch: when nothing
    // deletable remains (only the excluded in-progress recording is left
    // standing) report cannot-cleanup rather than the generic reached.
    bool onlyExcludedRemain = !deleteFailed && !snapshot.empty();
    for (const SnapshotEntry &entry : snapshot) {
      if (deletedPaths.contains(entry.path)) {
        continue;
      }
      if (!isExcluded(entry.path)) {
        onlyExcludedRemain = false;
        break;
      }
    }
    if (onlyExcludedRemain) {
      notifyLimitCannotCleanup(forceNotify);
    } else {
      notifyLimitReached(forceNotify);
    }
  }

  return !isOverLimit();
}

bool StorageManager::deleteRecording(const QString &filePath) {
  if (!deleteRecordingNoRefresh(filePath)) {
    return false;
  }
  refresh();
  return true;
}

bool StorageManager::deleteRecordingNoRefresh(const QString &filePath) {
  QFileInfo info(filePath);

  // Security check: ensure file and any symlink target are within storage.
  if (!isWithinStoragePath(filePath, m_storagePath)) {
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
    return true;
  }

  qWarning() << "Failed to delete recording:" << filePath << file.errorString();
  return false;
}

std::vector<StorageManager::SnapshotEntry>
StorageManager::snapshotRecordings() const {
  std::vector<SnapshotEntry> snapshot;
  QDirIterator it(m_storagePath,
                   QStringList() << "*.mkv" << "*.mp4" << "*.webm", QDir::Files,
                   QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    const QFileInfo info = it.fileInfo();
    snapshot.push_back(
        {info.absoluteFilePath(), info.lastModified(), info.size()});
  }
  // Same oldest-first order as getRecordingsSortedByDate().
  std::sort(snapshot.begin(), snapshot.end(),
            [](const SnapshotEntry &a, const SnapshotEntry &b) {
              return a.mtime < b.mtime;
            });
  return snapshot;
}

void StorageManager::deleteRecordings(const QStringList &filePaths) {
  if (filePaths.isEmpty()) {
    return;
  }

  // Snapshot for the worker: m_storagePath lives on the GUI thread.
  const QString storagePath = m_storagePath;
  const int total = filePaths.size();

  ++m_bulkDeletesActive;
  emit bulkDeleteInProgressChanged();
  // Emitted before the pool starts so observers (the library's
  // watcher suppression) are armed before the first unlink lands.
  emit bulkDeleteStarted();

  // Guarded by QPointer like Recording::loadMetadata: the manager is
  // app-owned, but a late completion must never touch a dead object.
  QPointer<StorageManager> self(this);
  QThreadPool::globalInstance()->start([self, filePaths, storagePath, total]() {
    QStringList deleted;
    int done = 0;
    for (const QString &filePath : filePaths) {
      if (self.isNull()) {
        // Manager destroyed mid-run: only possible at app teardown
        // (app-owned, never deleted otherwise). No terminal can be
        // delivered from a dead object, and none needs to be — the
        // library's bulk suppression dies with the process. (A queued
        // invokeMethod on a null receiver would be dropped by Qt
        // without running, so attempting one here would be a no-op.)
        return;
      }
      const QFileInfo info(filePath);
      // Same guards as deleteRecording(): stay inside the storage
      // path, skip missing files.
      if (!isWithinStoragePath(filePath, storagePath)) {
        qWarning() << "Refusing to delete file outside storage path:"
                   << filePath;
      } else if (!info.exists()) {
        qWarning() << "File does not exist:" << filePath;
      } else if (QFile::remove(filePath)) {
        const QString thumbPath = info.absolutePath() + "/.thumbnails/" +
                                  info.completeBaseName() + ".jpg";
        if (QFile::exists(thumbPath) && !QFile::remove(thumbPath)) {
          qWarning() << "Failed to delete thumbnail:" << thumbPath;
        }
        deleted.append(filePath);
      } else {
        qWarning() << "Failed to delete recording:" << filePath;
      }
      ++done;
      QMetaObject::invokeMethod(
          self,
          [self, done, total]() {
            if (!self.isNull()) {
              emit self->bulkDeleteProgress(done, total);
            }
          },
          Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(
        self,
        [self, deleted, total]() {
          if (!self.isNull()) {
            self->finishBulkDelete(deleted, total);
          }
        },
        Qt::QueuedConnection);
  });
}

void StorageManager::finishBulkDelete(const QStringList &deletedPaths,
                                      int totalCount) {
  qDebug() << "Bulk deleted" << deletedPaths.size() << "/" << totalCount
           << "recordings";
  if (!deletedPaths.isEmpty()) {
    emit recordingsDeleted(deletedPaths);
  }
  if (m_bulkDeletesActive > 0) {
    --m_bulkDeletesActive;
  }
  emit bulkDeleteInProgressChanged();
  refresh();
  emit bulkDeleteFinished(deletedPaths.size(), totalCount);
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

  // Security check: ensure file and any symlink target are within storage.
  if (!isWithinStoragePath(filePath, m_storagePath)) {
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
  QString newAbsolute = dir.absoluteFilePath(
      newBase + (suffix.isEmpty() ? QString() : "." + suffix));

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

bool StorageManager::exportRecording(const QString &sourcePath,
                                     const QString &destDir) const {
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
      destPath =
          dir.filePath(QString("%1_%2.%3").arg(baseName).arg(counter).arg(ext));
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
  if (m_bulkDeletesActive > 0) {
    // A bulk unlink storm would otherwise re-enter
    // enforceStorageLimit per file on a stale size (and could even
    // auto-delete an oldest file the user never selected). The bulk
    // completion settles with a single refresh instead.
    return;
  }
  // Debounce: restart the single-shot so a burst (a 50-file import fires
  // per file) settles into one enforceStorageLimit pass ~500ms after the
  // last event instead of one full rescan per file.
  m_debounceTimer.start();
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
  QStringList paths;
  QDirIterator it(m_storagePath,
                  QStringList() << "*.mkv" << "*.mp4" << "*.webm", QDir::Files,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    paths.append(it.fileInfo().absoluteFilePath());
  }
  // calculateDirectorySize() includes media below nested folders, so the
  // cleanup candidates must cover that same tree.
  std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
    return QFileInfo(a).lastModified() < QFileInfo(b).lastModified();
  });
  return paths;
}

} // namespace TrophyBackend
