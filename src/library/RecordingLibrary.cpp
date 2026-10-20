/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingLibrary.h"
#include "Recording.h"
#include "RecordingSidecars.h"
#include "RecordingAnnotations.h"
#include "ThumbnailGenerator.h"
#include "core/StorageManager.h"
#include "muxing/ReplayClipper.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <thread>

namespace TrophyBackend {

namespace {
constexpr char kCustomTitlesFile[] = ".trophy-titles.json";
constexpr char kCustomTitlesKey[] = "preferFileNameTitle";

bool isInsideLibrary(const QString &filePath, const QString &libraryPath) {
  const QFileInfo rootInfo(libraryPath);
  const QString lexicalRoot = QDir::cleanPath(rootInfo.absoluteFilePath());
  const QString resolvedRoot = rootInfo.canonicalFilePath();
  const QFileInfo candidateInfo(filePath);
  const QString candidate = QDir::cleanPath(candidateInfo.absoluteFilePath());
  const QString resolvedCandidate = candidateInfo.canonicalFilePath();
  if (lexicalRoot.isEmpty() || resolvedRoot.isEmpty() ||
      resolvedCandidate.isEmpty() || candidate == lexicalRoot)
    return false;

  auto isChildOf = [](const QString &path, const QString &root) {
    QString prefix = root;
    if (!prefix.endsWith(QDir::separator()))
      prefix += QDir::separator();
    return path.startsWith(prefix);
  };
  return isChildOf(candidate, lexicalRoot) &&
         isChildOf(resolvedCandidate, resolvedRoot);
}
} // namespace

RecordingLibrary::RecordingLibrary(const QString &libraryPath, QObject *parent)
    : QObject(parent), m_libraryPath(libraryPath),
      m_thumbnailGenerator(std::make_unique<ThumbnailGenerator>(this)) {
  // Connect thumbnail generator
  connect(m_thumbnailGenerator.get(), &ThumbnailGenerator::thumbnailReady, this,
          &RecordingLibrary::onThumbnailReady);

  // Watch for changes
  connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
          &RecordingLibrary::onDirectoryChanged);

  loadCustomTitles();

  // Initial scan
  refresh();
}

RecordingLibrary::~RecordingLibrary() {
  // The worker may still be returning after it queues its completion event.
  // Keep its captures alive until it has exited; a detached worker can race
  // QObject teardown (and process shutdown) even after clipSaved is emitted.
  if (m_clipThread.joinable())
    m_clipThread.join();
  if (m_storage && !m_clipPinnedSource.isEmpty())
    m_storage->releaseRecordings({m_clipPinnedSource});
}

QList<Recording *> RecordingLibrary::recordings() const {
  return m_filteredRecordings;
}

int RecordingLibrary::count() const { return m_filteredRecordings.size(); }

QList<Recording *> RecordingLibrary::allRecordings() const {
  QList<Recording *> result;
  for (const auto &rec : m_recordings) {
    if (rec->filePath() != m_activeRecordingPath)
      result.append(rec.get());
  }
  return result;
}

int RecordingLibrary::totalCount() const { return allRecordings().size(); }

void RecordingLibrary::setLibraryPath(const QString &path) {
  if (m_libraryPath != path) {
    // Remove old watch
    if (!m_libraryPath.isEmpty() &&
        m_watcher.directories().contains(m_libraryPath)) {
      m_watcher.removePath(m_libraryPath);
    }

    m_libraryPath = path;
    emit libraryPathChanged();
    loadCustomTitles();
    refresh();
  }
}

void RecordingLibrary::setSearchQuery(const QString &query) {
  if (m_searchQuery != query) {
    m_searchQuery = query;
    updateFilteredRecordings();
  }
}

void RecordingLibrary::setSortOrder(int order) {
  if (order < NewestFirst || order > NameZA)
    return;
  if (m_sortOrder != order) {
    m_sortOrder = order;
    updateFilteredRecordings();
  }
}

void RecordingLibrary::setKindFilter(int filter) {
  if (filter < AllCaptures || filter > NoCaptures || m_kindFilter == filter)
    return;
  m_kindFilter = filter;
  updateFilteredRecordings();
}

void RecordingLibrary::refresh() {
  // Batched so a scan settles the model with a single recordingsChanged
  // and at most one custom-titles write instead of one per file.
  beginBatch();
  scanDirectory();
  endBatch();
}

void RecordingLibrary::setProtectedOnly(bool value) {
  if (m_protectedOnly == value) return;
  m_protectedOnly = value;
  updateFilteredRecordings();
}

bool RecordingLibrary::setProtected(const QString &filePath, bool value) {
  auto *recording = getRecording(filePath);
  if (!recording || !isInsideLibrary(filePath, m_libraryPath) ||
      !RecordingAnnotations::setProtected(filePath, value)) return false;
  recording->reloadAnnotations();
  updateFilteredRecordings();
  return true;
}

bool RecordingLibrary::addBookmark(const QString &filePath, qint64 positionMs) {
  auto *recording = getRecording(filePath);
  if (!recording || !isInsideLibrary(filePath, m_libraryPath) ||
      // Library duration is truncated to whole seconds; the player clock
      // can legitimately bookmark the final fractional second.
      (recording->duration() > 0 && positionMs >= (recording->duration() + 1) * 1000) ||
      !RecordingAnnotations::addBookmark(filePath, positionMs)) return false;
  recording->reloadAnnotations();
  return true;
}

bool RecordingLibrary::removeBookmark(const QString &filePath, int index) {
  auto *recording = getRecording(filePath);
  if (!recording || !isInsideLibrary(filePath, m_libraryPath) ||
      !RecordingAnnotations::removeBookmark(filePath, index)) return false;
  recording->reloadAnnotations();
  return true;
}

Recording *RecordingLibrary::get(int index) const {
  if (index < 0 || index >= m_filteredRecordings.size()) {
    return nullptr;
  }
  return m_filteredRecordings[index];
}

Recording *RecordingLibrary::getRecording(const QString &filePath) const {
  for (const auto &rec : m_recordings) {
    if (rec->filePath() == filePath) {
      return rec.get();
    }
  }
  return nullptr;
}

QUrl RecordingLibrary::localFileUrl(const QString &filePath) const {
  return QUrl::fromLocalFile(filePath);
}

void RecordingLibrary::deleteRecording(const QString &filePath) {
  if (m_storage && m_storage->recordingInUse(filePath)) return;
  if (!isInsideLibrary(filePath, m_libraryPath)) {
    qWarning() << "Refusing to delete recording outside library:" << filePath;
    return;
  }
  const QString absolute = QFileInfo(filePath).absoluteFilePath();
  m_suppressedPaths.insert(absolute);
  QFile file(filePath);
  if (file.remove()) {
    RecordingAnnotations::remove(filePath);
    removeRecording(filePath);
  } else {
    m_suppressedPaths.remove(absolute);
    qWarning() << "Failed to delete recording:" << filePath
               << file.errorString();
  }
}

void RecordingLibrary::setActiveRecordingPath(const QString &path) {
  m_activeRecordingPath = path;
  updateFilteredRecordings();
}

void RecordingLibrary::setHiddenRecordings(const QStringList &paths) {
  m_hiddenRecordings = QSet<QString>(paths.begin(), paths.end());
  updateFilteredRecordings();
}

void RecordingLibrary::refreshAnnotations(const QString &path) {
  if (auto *recording = getRecording(path)) recording->reloadAnnotations();
}

void RecordingLibrary::saveClipRange(const QString &sourcePath,
                                     double startSeconds, double endSeconds) {
  auto fail = [&](const QString &reason) {
    qWarning() << "RecordingLibrary::saveClipRange:" << reason;
    emit clipSaveFailed(reason);
  };
  if (m_clipSaving) {
    fail(tr("A clip is already being saved."));
    return;
  }
  if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
    fail(tr("Source recording not found."));
    return;
  }
  if (!(endSeconds > startSeconds) || startSeconds < 0) {
    fail(tr("Pick a range on the timeline first."));
    return;
  }
  if (m_libraryPath.isEmpty()) {
    fail(tr("No library folder configured."));
    return;
  }
  // Storage pre-flight (same policy as ReplayManager::saveReplay):
  // refuse while over the limit, or when the new clip would push usage
  // over it. Check-only — clips never trigger auto-delete of library
  // files. The source file itself is already counted in usage; the clip
  // adds ~its range share on top (full size when the probed duration is
  // unknown, an upper bound that errs on the side of refusing).
  if (m_storage) {
    m_storage->refresh();
    if (m_storage->isOverLimit()) {
      fail(tr("Storage is full. Free space or enable auto-cleanup to save "
              "clips."));
      return;
    }
    if (m_storage->maxSize() > 0) {
      const qint64 sourceSize = QFileInfo(sourcePath).size();
      qint64 estimate = sourceSize;
      if (Recording *rec = getRecording(sourcePath)) {
        const qint64 dur = rec->duration();
        if (dur > 0) {
          estimate = static_cast<qint64>(sourceSize *
                                         (endSeconds - startSeconds) / dur);
          estimate = std::max<qint64>(1, std::min(estimate, sourceSize));
        }
      }
      if (m_storage->usedSize() + estimate > m_storage->maxSize()) {
        fail(tr("Not enough storage for this clip. Free space or enable "
                "auto-cleanup to save clips."));
        return;
      }
    }
  }
  // File under the source's game name so the clip badges like a DVR one.
  // Recording::gameName is the clean name ("Deadlock", never "Deadlock
  // Clip"); untracked files fall back to the filename stem.
  QString game;
  if (Recording *rec = getRecording(sourcePath))
    game = rec->gameName();
  if (game.isEmpty())
    game = QFileInfo(sourcePath).completeBaseName();
  QString safe = game;
  safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
  safe.replace(QRegularExpression("[_-]{2,}"), "_");
  safe = safe.trimmed().replace(QRegularExpression("^[_-]+|[_-]+$"), "");
  if (safe.isEmpty())
    safe = "Game";
  const QString ts =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
  const QString baseOut =
      QString("%1/%2_%3_Replay.mkv").arg(m_libraryPath, safe, ts);

  // Remux off the UI thread; completion hops back via queued invoke. The
  // clip is written to a .part file and atomically renamed: the library
  // watches this directory and must never see a half-written file (it
  // would generate a thumbnail for garbage).
  if (m_storage) {
    if (!m_storage->reserveRecordings({sourcePath})) {
      fail(tr("Source capture is in use or pending deletion."));
      return;
    }
    m_clipPinnedSource = sourcePath;
  }
  m_clipSaving = true;
  emit clipSavingChanged();
  // A prior worker may have queued completion immediately before it exited.
  // Reap it before reusing the single worker slot.
  if (m_clipThread.joinable())
    m_clipThread.join();
  QPointer<RecordingLibrary> self(this);
  const QString tmpOut = baseOut + "." +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                          ".part";
  const auto sourceAudioRouting = RecordingAnnotations::audioRouting(sourcePath);
  m_clipThread = std::thread(
       [self, sourcePath, startSeconds, endSeconds, game, baseOut, tmpOut, sourceAudioRouting]() {
        QString err;
        double keptSec = 0.0;
        bool ok = ReplayClipper::saveRange(sourcePath, startSeconds, endSeconds,
                                           tmpOut, &err, &keptSec);
        const double actualSec =
            keptSec > 0 ? keptSec : (endSeconds - startSeconds);
        QString out = baseOut;
        if (ok) {
          // rename() refuses an existing destination. Retry with a numeric
          // suffix so same-second saves (or another Trophy process) never
          // overwrite or spuriously fail on a name collision.
          const QFileInfo baseInfo(baseOut);
          const QString stem = baseInfo.completeBaseName();
          const QString suffix = baseInfo.suffix();
          for (int n = 0; n < 10000; ++n) {
            out = n == 0 ? baseOut
                         : baseInfo.absolutePath() + "/" + stem + "_" +
                               QString::number(n) + "." + suffix;
            if (QFile::rename(tmpOut, out))
              break;
            if (QFile::exists(out))
              continue;
            ok = false;
            break;
          }
          if (ok && QFile::exists(tmpOut))
            ok = false;
        }
        if (!ok)
          QFile::remove(tmpOut);
        if (!ok && err.isEmpty())
          err = "Could not save the clip file.";
        if (!ok)
          out.clear();
        if (ok && !RecordingAnnotations::setAudioRouting(out, sourceAudioRouting))
          qWarning() << "Could not copy clip audio source history:" << out;
        if (!self)
          return;
        QMetaObject::invokeMethod(
            self,
            [self, ok, out, game, err, actualSec]() {
              if (!self)
                return;
              self->m_clipSaving = false;
              if (self->m_storage && !self->m_clipPinnedSource.isEmpty()) {
                self->m_storage->releaseRecordings({self->m_clipPinnedSource});
                self->m_clipPinnedSource.clear();
              }
              emit self->clipSavingChanged();
              if (ok) {
                self->refreshAnnotations(out);
                emit self->clipSaved(out, game, actualSec);
              } else {
                emit self->clipSaveFailed(
                    err.isEmpty() ? self->tr("Could not save the clip.") : err);
              }
            },
            Qt::QueuedConnection);
      });
}

void RecordingLibrary::onRecordingRenamed(const QString &oldPath,
                                          const QString &newPath) {
  // Every in-app rename makes the new filename authoritative: the file
  // still carries the pre-rename container title tag.
  m_fileNameTitlePreferred.remove(QFileInfo(oldPath).fileName());
  if (!newPath.isEmpty()) {
    m_fileNameTitlePreferred.insert(QFileInfo(newPath).fileName());
    // Adopt the rename in place when the source is still tracked and
    // the target is not: the delegate keeps its live object (updated
    // via fileChanged/metadataChanged/thumbnailPathChanged) instead of
    // watching its recording get deleted and hoping the view rebuilds
    // the replacement in time. The refresh that follows this signal
    // then finds everything consistent and settles with no churn.
    // Anything else (untracked source, already-tracked target) falls
    // through to the remove + add scan path, which still needs the
    // deletion swallow below.
    if (Recording *recording = getRecording(oldPath);
        recording && !getRecording(newPath)) {
      recording->updateForRename(newPath);
      if (recording->thumbnailPath().isEmpty()) {
        m_thumbnailGenerator->generateThumbnail(newPath);
      }
    } else {
      // Mark the source so the refresh below doesn't report its
      // disappearance as a deletion (see removeRecording).
      m_renameSource = oldPath;
    }
  }
  saveCustomTitles();
}

void RecordingLibrary::onDirectoryChanged(const QString &path) {
  Q_UNUSED(path)
  if (m_bulkOperationDepth > 0) {
    // A background bulk delete is unlinking files; the bulk completion
    // path settles the model once (removeRecordings + refresh), so
    // per-file watcher refreshes here would just be churn.
    return;
  }
  if (!m_suppressedPaths.isEmpty()) {
    // The library itself just touched the suppressed paths
    // (deleteRecording removes the model entry synchronously, so the
    // follow-up watcher event is usually a no-op echo). The watcher only
    // names the directory, so diff the listing: swallow the event only
    // when nothing BUT self-touched paths changed. An unrelated file
    // added/removed in the same burst still refreshes.
    QSet<QString> current;
    const QFileInfoList files =
        QDir(m_libraryPath)
            .entryInfoList({"*.mkv", "*.mp4", "*.webm"}, QDir::Files);
    for (const QFileInfo &info : files) {
      current.insert(info.absoluteFilePath());
    }
    current.remove(m_activeRecordingPath);
    QSet<QString> known;
    for (const auto &rec : m_recordings) {
      if (rec) {
        known.insert(rec->filePath());
      }
    }
    QSet<QString> diff = current - known;
    diff.unite(known - current);
    bool onlySelf = true;
    for (const QString &changed : diff) {
      if (!m_suppressedPaths.contains(changed)) {
        onlySelf = false;
        break;
      }
    }
    // Suppressions for paths that now agree with the model are spent.
    QStringList spent;
    for (const QString &suppressed : m_suppressedPaths) {
      if (!diff.contains(suppressed)) {
        spent.append(suppressed);
      }
    }
    for (const QString &suppressed : spent) {
      m_suppressedPaths.remove(suppressed);
    }
    if (onlySelf) {
      return;
    }
  }
  refresh();
}

void RecordingLibrary::beginBatch() { ++m_batchDepth; }

void RecordingLibrary::endBatch() {
  if (m_batchDepth <= 0) {
    return;
  }
  if (--m_batchDepth > 0) {
    return;
  }
  if (m_titlesDirty) {
    m_titlesDirty = false;
    saveCustomTitles();
  }
  // Single settle + emit for the whole batch, preserving refresh()'s
  // trailing-emit contract (even a no-op scan notifies once).
  updateFilteredRecordings();
}

void RecordingLibrary::onThumbnailReady(const QString &videoPath,
                                        const QString &thumbnailPath) {
  for (auto &rec : m_recordings) {
    if (rec->filePath() == videoPath) {
      rec->setThumbnailPath(thumbnailPath);
      emit recordingsChanged();
      break;
    }
  }
}

void RecordingLibrary::scanDirectory() {
  QDir dir(m_libraryPath);

  if (!dir.exists()) {
    qDebug() << "Library path does not exist:" << m_libraryPath;
    if (!m_recordings.empty()) {
      m_recordings.clear();
      updateFilteredRecordings();
    }
    return;
  }

  // Add directory watch
  if (!m_watcher.directories().contains(m_libraryPath)) {
    m_watcher.addPath(m_libraryPath);
  }

  // Scan for video files
  QStringList filters = {"*.mkv", "*.mp4", "*.webm"};
  QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Time);

  // Also reclaim caches left by older versions or deletions while not running.
  RecordingSidecars::removeOrphans(m_libraryPath);

  // Identify files to remove
  QStringList currentFiles;
  for (const QFileInfo &fileInfo : files) {
    currentFiles << fileInfo.absoluteFilePath();
  }

  std::vector<QString> toRemove;
  for (const auto &rec : m_recordings) {
    if (!currentFiles.contains(rec->filePath())) {
      toRemove.push_back(rec->filePath());
    }
  }

  for (const auto &path : toRemove) {
    removeRecording(path);
  }

  // Add new files
  for (const QFileInfo &fileInfo : files) {
    addRecording(fileInfo.absoluteFilePath());
  }
}

void RecordingLibrary::addRecording(const QString &filePath) {
  // Skip active recording
  if (filePath == m_activeRecordingPath) {
    return;
  }

  // Check if already exists
  for (const auto &rec : m_recordings) {
    if (rec->filePath() == filePath) {
      return;
    }
  }

  auto recording = std::make_unique<Recording>(filePath, this);
  recording->setPreferFileNameTitle(
      m_fileNameTitlePreferred.contains(QFileInfo(filePath).fileName()));
  recording->loadMetadata();
  // Duration, date, title and clip identity arrive asynchronously. Reapply
  // the current view when probing completes (including renamed clips).
  connect(recording.get(), &Recording::metadataChanged, this,
          &RecordingLibrary::updateFilteredRecordings);

  // Request thumbnail generation
  if (recording->thumbnailPath().isEmpty()) {
    m_thumbnailGenerator->generateThumbnail(filePath);
  }

  m_recordings.push_back(std::move(recording));

  // Settle the filtered list before notifying: observers calling get()
  // inside the slot (the dashboard activity feed does) must never see a
  // stale list.
  updateFilteredRecordings();
  emit recordingAdded(m_recordings.back().get());
}

void RecordingLibrary::removeRecording(const QString &filePath) {
  // Drop any rename preference for the deleted file.
  if (m_fileNameTitlePreferred.remove(QFileInfo(filePath).fileName())) {
    saveCustomTitles();
  }

  RecordingSidecars::removeForVideo(filePath);

  auto it = std::remove_if(m_recordings.begin(), m_recordings.end(),
                           [&filePath](const std::unique_ptr<Recording> &rec) {
                             return rec->filePath() == filePath;
                           });

  if (it != m_recordings.end()) {
    m_recordings.erase(it, m_recordings.end());
    // Settle the filtered list BEFORE emitting: it still holds the raw
    // pointer that erase() just freed, and any observer calling get()
    // inside the slot would wrap a dangling QObject (live SIGSEGV,
    // 2026-10-01: deleting a library item crashed in QV4::fromData via
    // the new activity-feed onRecordingRemoved handler).
    updateFilteredRecordings();
    // A rename's source path vanishes from disk too, but that is not a
    // deletion — the renamed file arrives in the same refresh. Swallow
    // the signal (marked by onRecordingRenamed) so renames don't show
    // up as deletions in the activity feed.
    if (filePath == m_renameSource) {
      m_renameSource.clear();
    } else {
      emit recordingRemoved(filePath);
    }
  }
}

void RecordingLibrary::setBulkOperationActive(bool active) {
  if (active) {
    ++m_bulkOperationDepth;
  } else if (m_bulkOperationDepth > 0) {
    --m_bulkOperationDepth;
  }
}

void RecordingLibrary::removeRecordings(const QStringList &filePaths) {
  if (filePaths.isEmpty()) {
    return;
  }

  QSet<QString> doomed;
  for (const QString &path : filePaths) {
    doomed.insert(path);
  }

  // Which of the listed paths do we actually track? Only those are
  // reported, mirroring removeRecording (unknown files stay quiet).
  QSet<QString> known;
  for (const auto &rec : m_recordings) {
    if (rec && doomed.contains(rec->filePath())) {
      known.insert(rec->filePath());
    }
  }
  if (known.isEmpty()) {
    return;
  }

  // Drop any rename preference for the deleted files (one save).
  bool prefsChanged = false;
  for (const QString &path : known) {
    if (m_fileNameTitlePreferred.remove(QFileInfo(path).fileName())) {
      prefsChanged = true;
    }
    // Best-effort sidecar cleanup, mirroring removeRecording.
    RecordingSidecars::removeForVideo(path);
  }
  if (prefsChanged) {
    saveCustomTitles();
  }

  m_recordings.erase(
      std::remove_if(m_recordings.begin(), m_recordings.end(),
                     [&doomed](const std::unique_ptr<Recording> &rec) {
                       return rec && doomed.contains(rec->filePath());
                     }),
      m_recordings.end());

  // Settle the filtered list BEFORE emitting (same dangling-pointer
  // rule as removeRecording: observers calling get() in the slot must
  // never see freed entries).
  updateFilteredRecordings();

  // A rename's source path vanishing is not a deletion — same swallow
  // rule as removeRecording.
  QStringList removed;
  for (const QString &path : filePaths) {
    if (!known.contains(path)) {
      continue;
    }
    if (path == m_renameSource) {
      m_renameSource.clear();
      continue;
    }
    removed.append(path);
  }
  if (!removed.isEmpty()) {
    emit recordingsRemoved(removed);
  }
}

void RecordingLibrary::updateFilteredRecordings() {
  m_filteredRecordings.clear();

  for (const auto &rec : m_recordings) {
    // Skip active recording
    if (rec->filePath() == m_activeRecordingPath) {
      continue;
    }

    // Apply search filter
    if (m_hiddenRecordings.contains(rec->filePath()) ||
        (m_protectedOnly && !rec->protectedFromCleanup()) || m_kindFilter == NoCaptures ||
        (m_kindFilter == RecordingsOnly && rec->isClip()) ||
        (m_kindFilter == ClipsOnly && !rec->isClip())) {
      continue;
    }

    if (!m_searchQuery.isEmpty()) {
      if (!rec->gameName().contains(m_searchQuery, Qt::CaseInsensitive) &&
          !rec->fileName().contains(m_searchQuery, Qt::CaseInsensitive)) {
        continue;
      }
    }

    m_filteredRecordings.append(rec.get());
  }

  // A path tie-break keeps equal dates/sizes/durations deterministic as
  // background metadata arrives and the view is rebuilt.
  std::sort(m_filteredRecordings.begin(), m_filteredRecordings.end(),
            [this](Recording *a, Recording *b) {
    int comparison = 0;
    const auto compare = [](auto x, auto y) { return x < y ? -1 : x > y ? 1 : 0; };
    switch (m_sortOrder) {
    case NewestFirst: comparison = compare(b->date(), a->date()); break;
    case OldestFirst: comparison = compare(a->date(), b->date()); break;
    case LargestFirst: comparison = compare(b->size(), a->size()); break;
    case SmallestFirst: comparison = compare(a->size(), b->size()); break;
    case LongestFirst: comparison = compare(b->duration(), a->duration()); break;
    case ShortestFirst: comparison = compare(a->duration(), b->duration()); break;
    case NameAZ: comparison = a->gameName().compare(b->gameName(), Qt::CaseInsensitive); break;
    case NameZA: comparison = b->gameName().compare(a->gameName(), Qt::CaseInsensitive); break;
    }
    return comparison != 0 ? comparison < 0 : a->filePath() < b->filePath();
  });

  if (m_batchDepth > 0) {
    // Inside a scan/refresh batch: the list above is settled (callers
    // emitting per-file signals still see a current model), but the
    // broadcast waits for endBatch so a 100-file scan costs one rebuild.
    return;
  }
  emit recordingsChanged();
}

QString RecordingLibrary::customTitlesPath() const {
  return QDir(m_libraryPath).absoluteFilePath(kCustomTitlesFile);
}

void RecordingLibrary::loadCustomTitles() {
  m_fileNameTitlePreferred.clear();

  QFile file(customTitlesPath());
  if (!file.exists()) {
    return;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "Failed to read custom titles:" << file.fileName();
    return;
  }
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject()) {
    qWarning() << "Ignoring malformed custom titles:" << file.fileName();
    return;
  }
  const QJsonArray names = doc.object().value(kCustomTitlesKey).toArray();
  for (const QJsonValue &v : names) {
    if (v.isString() && !v.toString().isEmpty()) {
      m_fileNameTitlePreferred.insert(v.toString());
    }
  }
}

void RecordingLibrary::saveCustomTitles() const {
  if (m_batchDepth > 0) {
    // Inside a scan/refresh batch: coalesce per-remove writes into the
    // single write endBatch performs.
    m_titlesDirty = true;
    return;
  }
  if (m_libraryPath.isEmpty()) {
    return;
  }
  QDir dir(m_libraryPath);
  if (!dir.exists() && !dir.mkpath(".")) {
    qWarning() << "Failed to create library path for custom titles:"
               << m_libraryPath;
    return;
  }
  QJsonArray names;
  for (const QString &name : m_fileNameTitlePreferred) {
    names.append(name);
  }
  QJsonObject root;
  root.insert(kCustomTitlesKey, names);

  QSaveFile file(customTitlesPath());
  if (!file.open(QIODevice::WriteOnly)) {
    qWarning() << "Failed to write custom titles:" << file.fileName();
    return;
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  if (!file.commit()) {
    qWarning() << "Failed to commit custom titles:" << file.fileName();
  }
}

} // namespace TrophyBackend
