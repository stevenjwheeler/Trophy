/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ShareExporter.h"
#include "core/StorageManager.h"
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace {
// Worker-only bounded copy. Publish only a complete, unchanged source and never
// overwrite a destination. The temporary lives on the destination filesystem.
TrophyBackend::ShareExportResult copyOriginalFile(const QString &source, const QString &directory,
    std::atomic_bool &cancel, std::atomic<double> &progress) {
  if (cancel.load()) return {};
  QFile input(source);
  const int fd = ::open(QFile::encodeName(source).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return {{}, QString::fromLocal8Bit(std::strerror(errno))};
  if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    ::close(fd);
    return {{}, input.errorString()};
  }
  struct stat before {};
  if (::fstat(input.handle(), &before) != 0 || !S_ISREG(before.st_mode))
    return {{}, QObject::tr("Cannot read the original capture.")};
  QTemporaryFile output(QDir(directory).filePath(".trophy-copy-XXXXXX.part"));
  if (!output.open()) return {{}, output.errorString()};
  QByteArray buffer(1024 * 1024, Qt::Uninitialized);
  qint64 copied = 0;
  while (copied < before.st_size) {
    if (cancel.load()) return {};
    const qint64 read = input.read(buffer.data(), qMin<qint64>(buffer.size(), before.st_size - copied));
    if (read <= 0) return {{}, QObject::tr("Could not read the complete original capture.")};
    qint64 written = 0;
    while (written < read) {
      if (cancel.load()) return {};
      const qint64 count = output.write(buffer.constData() + written, read - written);
      if (count <= 0) return {{}, output.errorString()};
      written += count;
    }
    copied += read;
    progress.store(qMin(0.99, double(copied) / double(qMax<qint64>(1, before.st_size))));
  }
  if (!output.flush()) return {{}, output.errorString()};
  output.close();
  if (output.error() != QFileDevice::NoError) return {{}, output.errorString()};
  struct stat after {}, current {};
  auto same = [&before](const struct stat &value) {
    return S_ISREG(value.st_mode) && value.st_dev == before.st_dev && value.st_ino == before.st_ino &&
        value.st_size == before.st_size && value.st_mtim.tv_sec == before.st_mtim.tv_sec &&
        value.st_mtim.tv_nsec == before.st_mtim.tv_nsec;
  };
  if (::fstat(input.handle(), &after) != 0 || ::lstat(QFile::encodeName(source).constData(), &current) != 0 ||
      !same(after) || !same(current))
    return {{}, QObject::tr("The original capture changed while copying. Please try again.")};
  const QFileInfo file(source);
  QDir destination(directory);
  for (int suffix = 0; suffix < 10000; ++suffix) {
    if (cancel.load()) return {};
    const QString name = suffix == 0 ? file.fileName() : file.completeBaseName() + "_" + QString::number(suffix)
        + (file.suffix().isEmpty() ? QString{} : "." + file.suffix());
    const auto path = destination.filePath(name);
    const QFileInfo existing(path);
    if (existing.exists() || existing.isSymLink()) continue;
    if (output.rename(path)) {
      output.setAutoRemove(false);
      progress.store(1.0);
      return {path, {}};
    }
    // A concurrent creator must not be overwritten. Other I/O errors fail.
    if (!QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) return {{}, output.errorString()};
  }
  return {{}, QObject::tr("Too many copies with the same name in this folder.")};
}
}

namespace TrophyBackend {
ShareExporter::ShareExporter(StorageManager *storage, QObject *parent)
    : QObject(parent), m_storage(storage) {
  m_timer.setInterval(100);
  connect(&m_timer, &QTimer::timeout, this, &ShareExporter::poll);
}
ShareExporter::~ShareExporter() {
  m_cancel->store(true);
  for (auto &job : m_jobs) job->cancel->store(true);
  if (m_worker.valid()) m_worker.wait();
  for (auto &job : m_jobs) {
    if (job->worker.valid()) job->worker.wait();
    unpinSource(job->source);
  }
  unpin();
}
double ShareExporter::progress() const {
  return m_jobs.empty() ? 0.0 : m_jobs.front()->progress->load();
}
QVariantList ShareExporter::jobs() const {
  QVariantList rows;
  for (const auto &job : m_jobs)
    rows.append(QVariantMap{{"id", job->id}, {"sourcePath", job->source},
        {"active", job->started}, {"progress", job->progress->load()},
        {"cancelling", job->cancel->load()}, {"copyOriginal", job->copyOriginal}});
  return rows;
}
bool ShareExporter::pin(const QString &source) {
  // StorageManager reservations are exclusive, not reference-counted. Share one
  // reservation across our probe and queued jobs for the same capture.
  if (!m_pins.contains(source) && !m_storage->reserveRecordings({source})) return false;
  ++m_pins[source];
  return true;
}
void ShareExporter::unpinSource(const QString &source) {
  auto found = m_pins.find(source);
  if (found == m_pins.end()) return;
  if (--found.value() == 0) {
    m_pins.erase(found);
    m_storage->releaseRecordings({source});
  }
}
void ShareExporter::unpin() {
  if (m_pinned) unpinSource(m_source);
  m_pinned = false;
}
bool ShareExporter::inspect(const QString &source) {
  if (m_worker.valid()) return false;
  unpin();
  const QFileInfo file(source);
  m_source = file.canonicalFilePath();
  if (m_source.isEmpty() || file.isSymLink()) m_source = file.absoluteFilePath();
  m_error.clear(); m_result.clear(); m_info.clear();
  if (file.isSymLink() || !pin(m_source)) {
    m_error = tr("Capture is unavailable or in use. Finish recording/deletion before exporting.");
    emit changed(); return false;
  }
  m_pinned = true; m_inspecting = true; m_releaseAfterWork = false;
  m_cancel = std::make_shared<std::atomic_bool>(false);
  const auto stop = m_cancel;
  const auto path = m_source;
  try {
    m_worker = std::async(std::launch::async, [path, stop] {
      setpriority(PRIO_PROCESS, 0, 10);
      return Result{ShareExportEngine::inspect(path, *stop)};
    });
  } catch (const std::exception &e) {
    m_inspecting = false; unpin(); m_error = QString::fromUtf8(e.what()); emit changed(); return false;
  }
  m_timer.start(); emit changed(); return true;
}
bool ShareExporter::start(const QString &directory, int height, qint64 bytes, const QVariantList &tracks, bool watermark) {
  if (m_worker.valid() || m_source.isEmpty() || m_info.contains("error")) return false;
  const QString root = QFileInfo(m_storage->storagePath()).canonicalFilePath();
  const QString destination = QFileInfo(directory).canonicalFilePath();
  const QString prefix = root.endsWith(QDir::separator()) ? root : root + QDir::separator();
  if (!root.isEmpty() && (destination == root || destination.startsWith(prefix))) {
    m_error = tr("Choose an export folder outside the managed recording library, so auto-cleanup cannot remove your share copy or original to make room.");
    emit changed(); return false;
  }
  if (!m_pinned && !pin(m_source)) {
    m_error = tr("Capture is no longer available for export."); emit changed(); return false;
  }
  m_pinned = true;
  ShareExportOptions options;
  options.watermark = watermark;
  options.maxHeight = height; options.targetBytes = bytes;
  for (const auto &track : tracks) {
    bool ok;
    const int index = track.toInt(&ok);
    if (!ok) { m_error = tr("Invalid audio selection."); emit changed(); return false; }
    options.audioStreams.append(index);
  }
  auto job = std::make_unique<Job>();
  job->id = QString::number(++m_nextJobId);
  job->source = m_source;
  job->directory = directory;
  job->options = options;
  m_jobs.push_back(std::move(job));
  m_pinned = false; // Transfer the probe's reservation to the queued job.
  m_releaseAfterWork = false; m_error.clear(); m_result.clear();
  startNext();
  m_timer.start(); emit changed(); return true;
}
bool ShareExporter::startBatch(const QString &directory, int height, qint64 bytes, const QVariantList &captures, bool watermark) {
  if (m_worker.valid() || captures.isEmpty()) return false;
  const QString root = QFileInfo(m_storage->storagePath()).canonicalFilePath();
  const QString destination = QFileInfo(directory).canonicalFilePath();
  const QString prefix = root.endsWith(QDir::separator()) ? root : root + QDir::separator();
  if (destination.isEmpty() || !QFileInfo(destination).isDir() ||
      (!root.isEmpty() && (destination == root || destination.startsWith(prefix)))) {
    m_error = tr("Choose an existing export folder outside the managed recording library.");
    emit changed(); return false;
  }
  // Stage and reserve the whole batch before publishing any job. A missing or
  // reserved input must not silently export only part of the user's selection.
  std::deque<std::unique_ptr<Job>> staged;
  QStringList pinned;
  for (const auto &capture : captures) {
    const auto row = capture.toMap();
    const QFileInfo file(row.value("sourcePath").toString());
    auto job = std::make_unique<Job>();
    job->source = file.canonicalFilePath();
    job->directory = destination;
    job->options.maxHeight = height;
    job->options.targetBytes = bytes;
    job->options.watermark = watermark;
    bool valid = row.contains("audioStreams") && row.value("audioStreams").canConvert<QVariantList>();
    for (const auto &track : row.value("audioStreams").toList()) {
      bool ok = false;
      const int index = track.toInt(&ok);
      valid = valid && ok && index >= 0 && !job->options.audioStreams.contains(index);
      job->options.audioStreams.append(index);
    }
    valid = valid && job->options.audioStreams.size() <= 8;
    if (!valid || file.isSymLink() || !file.isFile() || job->source.isEmpty() || !pin(job->source)) {
      for (const auto &path : pinned) unpinSource(path);
      m_error = tr("Cannot queue %1. Check its audio selection and that the capture is available and not in use.").arg(file.fileName());
      emit changed(); return false;
    }
    pinned.append(job->source);
    staged.push_back(std::move(job));
  }
  for (auto &job : staged) {
    job->id = QString::number(++m_nextJobId);
    m_jobs.push_back(std::move(job));
  }
  m_error.clear(); m_result.clear();
  startNext();
  m_timer.start(); emit changed(); return true;
}
bool ShareExporter::copyOriginals(const QStringList &sources, const QString &directory) {
  if (sources.isEmpty()) return false;
  const QString root = QFileInfo(m_storage->storagePath()).canonicalFilePath();
  const QString destination = QFileInfo(directory).canonicalFilePath();
  const QString prefix = root.endsWith(QDir::separator()) ? root : root + QDir::separator();
  if (destination.isEmpty() || !QFileInfo(destination).isDir() ||
      (!root.isEmpty() && (destination == root || destination.startsWith(prefix)))) {
    m_error = tr("Choose an existing copy folder outside the managed recording library.");
    emit changed(); return false;
  }
  std::deque<std::unique_ptr<Job>> staged;
  QStringList pinned;
  for (const auto &source : sources) {
    const QFileInfo file(source);
    auto job = std::make_unique<Job>();
    job->source = file.canonicalFilePath();
    job->directory = destination;
    job->copyOriginal = true;
    if (file.isSymLink() || !file.isFile() || job->source.isEmpty() || !pin(job->source)) {
      for (const auto &path : pinned) unpinSource(path);
      m_error = tr("Cannot copy %1. Check that the capture is available and not in use.").arg(file.fileName());
      emit changed(); return false;
    }
    pinned.append(job->source);
    staged.push_back(std::move(job));
  }
  for (auto &job : staged) {
    job->id = QString::number(++m_nextJobId);
    m_jobs.push_back(std::move(job));
  }
  m_error.clear(); m_result.clear();
  startNext();
  m_timer.start(); emit changed(); return true;
}
void ShareExporter::startNext() {
  if (m_jobs.empty() || m_jobs.front()->started) return;
  auto &job = *m_jobs.front();
  job.started = true;
  const auto path = job.source;
  const auto directory = job.directory;
  const auto options = job.options;
  const auto stop = job.cancel;
  const auto progress = job.progress;
  const bool copyOriginal = job.copyOriginal;
  try {
    job.worker = std::async(std::launch::async, [path, directory, options, stop, progress, copyOriginal] {
      setpriority(PRIO_PROCESS, 0, 10);
      return copyOriginal ? copyOriginalFile(path, directory, *stop, *progress)
          : ShareExportEngine::run(path, directory, options, *stop, *progress);
    });
  } catch (const std::exception &e) {
    job.launchError = QString::fromUtf8(e.what());
  }
}
void ShareExporter::cancel() {
  if (!m_jobs.empty()) cancelJob(m_jobs.front()->id);
  else m_cancel->store(true);
}
void ShareExporter::cancelJob(const QString &id) {
  for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) {
    if ((*it)->id != id) continue;
    if ((*it)->started) (*it)->cancel->store(true);
    else {
      unpinSource((*it)->source);
      m_jobs.erase(it);
    }
    emit changed();
    return;
  }
}
QString ShareExporter::localFolderPath(const QUrl &url) const {
  return url.isLocalFile() ? url.toLocalFile() : QString{};
}
QUrl ShareExporter::resultFolder() const {
  return m_result.isEmpty() ? QUrl{} : QUrl::fromLocalFile(QFileInfo(m_result).absolutePath());
}
void ShareExporter::release() {
  if (m_worker.valid()) { m_releaseAfterWork = true; m_cancel->store(true); return; }
  unpin(); m_source.clear(); m_info.clear(); emit changed();
}
void ShareExporter::poll() {
  if (m_worker.valid() && m_worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    Result result;
    try { result = m_worker.get(); }
    catch (const std::exception &e) { result.info["error"] = QString::fromUtf8(e.what()); }
    m_inspecting = false;
    m_info = result.info; m_error = result.info.value("error").toString();
    if (!m_error.isEmpty() || m_releaseAfterWork) unpin();
    if (m_releaseAfterWork) { m_source.clear(); m_info.clear(); }
    // Publish probe completion independently of the running export.
    emit changed();
  }
  QString completedPath, failedReason, settledId, settledSource;
  bool cancelled = false, copyOriginal = false;
  if (!m_jobs.empty()) {
    auto &job = *m_jobs.front();
    if (!job.launchError.isEmpty() || (job.worker.valid() &&
        job.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) {
      ShareExportResult result;
      try {
        if (job.worker.valid()) result = job.worker.get();
        else result.error = job.launchError;
      } catch (const std::exception &e) { result.error = QString::fromUtf8(e.what()); }
      completedPath = result.path;
      settledId = job.id;
      settledSource = job.source;
      cancelled = job.cancel->load();
      copyOriginal = job.copyOriginal;
      if (!job.cancel->load()) failedReason = result.error;
      m_result = result.path;
      if (m_source == job.source && !m_inspecting && !m_pinned) m_error = result.error;
      unpinSource(job.source);
      m_jobs.pop_front();
      startNext();
    }
  }
  if (!m_worker.valid() && m_jobs.empty()) m_timer.stop();
  if (!settledId.isEmpty())
    emit jobSettled(settledId, settledSource, completedPath, failedReason, cancelled, copyOriginal);
  emit changed();
  if (!completedPath.isEmpty()) emit finished(completedPath);
  else if (!failedReason.isEmpty()) emit failed(failedReason);
}
}
