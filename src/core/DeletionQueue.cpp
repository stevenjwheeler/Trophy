/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "DeletionQueue.h"
#include "StorageManager.h"
#include <QFileInfo>
#include <QFile>
#include <QUuid>
#include <algorithm>
#include <sys/stat.h>

namespace TrophyBackend {
namespace {
QString identity(const QString &path) {
  struct stat s {};
  if (::lstat(QFile::encodeName(path).constData(), &s) != 0 || !S_ISREG(s.st_mode)) return {};
  return QString("%1:%2:%3:%4:%5").arg(qulonglong(s.st_dev)).arg(qulonglong(s.st_ino))
      .arg(qlonglong(s.st_size)).arg(qlonglong(s.st_mtim.tv_sec)).arg(qlonglong(s.st_mtim.tv_nsec));
}
}
DeletionQueue::DeletionQueue(StorageManager *storage, QObject *parent)
    : QObject(parent), m_storage(storage) {
  m_clock.start();
  m_now = [this] { return m_clock.elapsed(); };
  m_timer.setInterval(50);
  connect(&m_timer, &QTimer::timeout, this, &DeletionQueue::tick);
  connect(storage, &StorageManager::bulkDeleteFinished, this, &DeletionQueue::completed);
}
DeletionQueue::~DeletionQueue() {
  // Closing before expiry cancels pending deletion, never accelerates it.
  for (const auto &batch : m_batches)
    if (!batch.committing) m_storage->releaseRecordings(batch.paths);
}
QVariantList DeletionQueue::pending() const {
  QVariantList rows;
  for (const auto &batch : m_batches) rows.append(describe(batch));
  return rows;
}
QVariantMap DeletionQueue::describe(const Batch &batch) const {
  const qint64 elapsed = std::clamp(m_now() - batch.started, qint64(0), qint64(kUndoMs));
  return {{"id", batch.id}, {"count", batch.paths.size()},
        {"label", batch.paths.size() == 1 ? QFileInfo(batch.paths.first()).fileName() : tr("%1 captures").arg(batch.paths.size())},
        {"progress", double(elapsed) / kUndoMs},
        {"secondsLeft", (kUndoMs - elapsed + 999) / 1000},
        {"committing", batch.committing}};
}
QVariantMap DeletionQueue::status(const QString &id) const {
  for (const auto &batch : m_batches) if (batch.id == id) return describe(batch);
  return {};
}
bool DeletionQueue::enqueue(const QStringList &input) {
  QStringList paths;
  for (const auto &path : input) paths.append(QFileInfo(path).absoluteFilePath());
  paths.removeDuplicates();
  if (!m_storage->reserveRecordings(paths)) {
    emit failed(tr("Cannot delete: a capture is recording, exporting, already pending deletion, or unavailable."));
    return false;
  }
  Batch batch{QUuid::createUuid().toString(QUuid::WithoutBraces), paths, m_now(), false, {}};
  for (const auto &path : paths) {
    const auto stamp = identity(path);
    if (stamp.isEmpty()) {
      m_storage->releaseRecordings(paths);
      emit failed(tr("A capture changed before deletion could be queued."));
      return false;
    }
    batch.identities.insert(path, stamp);
  }
  m_batches.append(batch);
  m_timer.start();
  publish();
  return true;
}
bool DeletionQueue::undo(const QString &id) {
  for (auto it = m_batches.begin(); it != m_batches.end(); ++it) {
    if (it->id != id || it->committing) continue;
    m_storage->releaseRecordings(it->paths);
    m_batches.erase(it);
    publish();
    return true;
  }
  return false;
}
void DeletionQueue::publish() {
  QStringList hidden;
  for (const auto &batch : m_batches) hidden.append(batch.paths);
  emit hiddenPathsChanged(hidden);
  emit pendingChanged();
  if (m_batches.isEmpty()) m_timer.stop();
}
void DeletionQueue::tick() {
  if (m_storage->bulkDeleteInProgress()) return;
  for (auto it = m_batches.begin(); it != m_batches.end(); ++it) {
    if (it->committing || m_now() - it->started < kUndoMs) continue;
    for (const auto &path : it->paths) {
      if (identity(path) == it->identities.value(path)) continue;
      m_storage->releaseRecordings(it->paths);
      m_batches.erase(it);
      publish();
      emit failed(tr("Deletion cancelled because a capture changed or moved. Remaining files are back in the library."));
      return;
    }
    it->committing = true;
    m_storage->releaseRecordings(it->paths);
    m_storage->deleteRecordings(it->paths);
    return;
  }
}
void DeletionQueue::completed(int deleted, int total) {
  for (auto it = m_batches.begin(); it != m_batches.end(); ++it) {
    if (!it->committing) continue;
    m_batches.erase(it);
    publish();
    if (deleted != total) emit failed(tr("Some captures could not be deleted. Remaining files are back in the library."));
    return;
  }
}
}
