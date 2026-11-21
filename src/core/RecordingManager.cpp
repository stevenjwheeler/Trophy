/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "RecordingManager.h"
#include "StorageManager.h"
#include "settings/SettingsManager.h"
#include "library/RecordingAnnotations.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/TrophyMetadata.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <utility>

namespace TrophyBackend {
RecordingManager::FileOutput::FileOutput(QString directory) : m_directory(std::move(directory)) {}
RecordingManager::FileOutput::~FileOutput() = default;
bool RecordingManager::FileOutput::begin(const EncodedStream &stream) {
  m_stream = stream;
  m_open = false;
  m_started.store(false);
  m_duration.store(0);
  m_position.store(-1);
  m_bytes.store(0);
  std::lock_guard lock(m_mutex);
  m_path = std::exchange(m_nextPath, {});
  m_error.clear();
  return true;
}
bool RecordingManager::FileOutput::open() {
  if (!QDir().mkpath(m_directory)) {
    std::lock_guard lock(m_mutex);
    m_error = QStringLiteral("Failed to create recording directory");
    return false;
  }
  auto path = this->path();
  if (path.isEmpty()) path = StorageManager::newRecordingPath(m_directory, m_stream.game);
  m_muxer = FFmpegMuxer::createShared();
  auto metadata = m_stream.metadata;
  stampTrophyTags(metadata, QStringLiteral("recording"));
  bool ok = m_muxer->initialize(path) && m_muxer->setMetadata(metadata);
  for (size_t i = 0; ok && i < m_stream.streams.size(); ++i)
    ok = m_muxer->addStream(m_stream.streams[i]) == int(i);
  if (ok) ok = m_muxer->writeHeader();
  {
    std::lock_guard lock(m_mutex);
    m_path = path;
    if (!ok) m_error = m_muxer->lastError().isEmpty()
        ? QStringLiteral("Failed to open recording output") : m_muxer->lastError();
  }
  m_open = ok;
  return ok;
}
bool RecordingManager::FileOutput::write(const EncodedFrame &frame) {
  if (!m_open) {
    if (frame->streamIndex != 0 || !frame->isKeyframe) return true;
    m_origin = frame->pts;
    if (!open()) return false;
  }
  if (frame->pts < m_origin) return true;
  auto packet = *frame;
  packet.pts -= m_origin;
  packet.dts -= m_origin;
  if (!m_muxer->writePacket(packet)) {
    std::lock_guard lock(m_mutex);
    m_error = m_muxer->lastError().isEmpty() ? QStringLiteral("Failed to write recording output")
                                            : m_muxer->lastError();
    return false;
  }
  if (frame->streamIndex == 0) {
    m_duration.store(std::max<qint64>(m_duration.load(), packet.pts + std::max<int64_t>(1, packet.duration)));
    m_position.store(packet.pts);
  }
  m_bytes.store(m_muxer->bytesWritten());
  m_started.store(true);
  return true;
}
bool RecordingManager::FileOutput::end() {
  if (!m_open) return error().isEmpty();
  const bool ok = m_muxer->finalize();
  std::lock_guard lock(m_mutex);
  if (!ok && m_error.isEmpty())
    m_error = m_muxer->lastError().isEmpty() ? QStringLiteral("Failed to finalize recording output")
                                           : m_muxer->lastError();
  m_bytes.store(m_muxer->bytesWritten());
  m_parts.append({m_stream.generation, m_path, m_duration.load(), m_bytes.load(),
                  ok && m_error.isEmpty(), {}});
  m_open = false;
  m_muxer.reset();
  return ok && m_error.isEmpty();
}
QString RecordingManager::FileOutput::error() const { std::lock_guard lock(m_mutex); return m_error; }
QString RecordingManager::FileOutput::path() const {
  std::lock_guard lock(m_mutex);
  return m_nextPath.isEmpty() ? m_path : m_nextPath;
}
QString RecordingManager::FileOutput::preparePath(const QString &game) {
  const auto path = StorageManager::newRecordingPath(m_directory, game);
  std::lock_guard lock(m_mutex);
  m_nextPath = path;
  return path;
}
QList<RecordingManager::FileOutput::Part> RecordingManager::FileOutput::takeParts() {
  std::lock_guard lock(m_mutex);
  return std::exchange(m_parts, {});
}
void RecordingManager::FileOutput::setAudioHistory(quint64 generation, const QJsonObject &history) {
  std::lock_guard lock(m_mutex);
  for (auto &part : m_parts) if (part.generation == generation) part.history = history;
}
qint64 RecordingManager::recordingDuration() const { return m_output ? m_output->durationMs() : 0; }
qint64 RecordingManager::recordingSize() const { return m_output ? m_output->bytes() : 0; }
QString RecordingManager::currentFilePath() const { return m_output ? m_output->path() : QString(); }
RecordingManager::RecordingManager(QObject *parent)
    : RecordingManager(std::make_shared<RecordingEngine>(), parent) {}
RecordingManager::RecordingManager(std::shared_ptr<RecordingEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine)) {
  connect(m_engine.get(), &RecordingEngine::captureSelectionFinished,
          this, &RecordingManager::captureSelectionFinished);
  connect(m_engine.get(), &RecordingEngine::captureRecoveryChanged, this, [this] {
    emit captureRecoveryChanged();
    if (captureResumeRequired()) emit captureResumeOffered(m_game);
  });
  connect(m_engine.get(), &RecordingEngine::stateChanged, this, [this] {
    const auto game = m_engine->currentGame();
    if (m_id && m_state == RecordingState::Initializing && !game.isEmpty() && m_game != game) {
      m_game = game;
      preparePart();
      emit currentGameNameChanged();
    }
    emit activeEncoderNameChanged();
  });
  connect(m_engine.get(), &RecordingEngine::restoreAuthNeeded, this,
          [this](quint64 id, const QString &game, const QString &identity) {
    if (id == m_id) emit restoreAuthNeeded(game, identity);
  });
  connect(m_engine.get(), &RecordingEngine::outputStarted, this, [this](quint64 id) {
    if (id != m_id || m_state == RecordingState::Stopping) return;
    m_game = m_engine->currentGame();
    emit currentGameNameChanged();
    setState(RecordingState::Recording);
    if (m_storage) m_storage->setExcludedRecordingPath(currentFilePath());
    emit recordingStarted(currentFilePath());
  });
  connect(m_engine.get(), &RecordingEngine::progressUpdated, this, [this] {
    if (m_id) emit progressUpdated(recordingDuration(), recordingSize());
  });
  connect(m_engine.get(), &RecordingEngine::outputError, this,
          [this](quint64 id, const QString &error) {
    if (id != m_id) return;
    m_failed = true;
    m_errorReported = true;
    emit recordingError(error);
  });
  connect(m_engine.get(), &RecordingEngine::generationEnded, this,
          [this](quint64 id, bool ok, bool continuing, RecordingEngine::EndReason reason) {
    if (id != m_id) return;
    m_failed |= !ok;
    m_quietStop = continuing || reason == RecordingEngine::EndReason::PermissionClosed;
    fileParts(m_quietStop);
    if (continuing) {
      preparePart();
      setState(RecordingState::Initializing);
    }
  });
  connect(m_engine.get(), &RecordingEngine::outputStopped, this,
          [this](quint64 id, bool ok, const QString &error) {
    if (id != m_id) return;
    m_failed |= !ok;
    fileParts(m_quietStop);
    if (!m_failed) completeFeedback();
    else m_deferred = {};
    if (!ok && !m_errorReported)
      emit recordingError(error.isEmpty() ? QStringLiteral("Failed to finalize recording") : error);
    m_id = 0;
    if (m_storage) { m_storage->clearExcludedRecordingPath(); m_storage->refresh(); }
    m_output.reset();
    setState(RecordingState::Idle);
    emit recordingFinished();
  });
}
RecordingManager::~RecordingManager() {
  disconnect(m_engine.get(), nullptr, this, nullptr);
  if (m_id) m_engine->detach(m_id);
}
void RecordingManager::setSettingsManager(SettingsManager *settings) {
  m_settings = settings;
  m_engine->setSettingsManager(settings);
}
bool RecordingManager::isRecoveringCapture() const {
  return m_id && m_engine->state() == RecordingEngine::State::Recovering;
}
void RecordingManager::setDetectedEncoderName(const QString &name) {
  if (m_detectedEncoder == name) return;
  m_detectedEncoder = name;
  emit detectedEncoderNameChanged();
}
void RecordingManager::setState(RecordingState state) {
  if (m_state == state) return;
  m_state = state;
  emit stateChanged(state);
}
void RecordingManager::startRecording(const QString &game) { start(game, false); }
void RecordingManager::start(const QString &game, bool fresh) {
  if (m_id || !m_settings || !m_storage) return;
  if (!m_storage->enforceStorageLimit(false, true)) {
    emit recordingError(QStringLiteral("Storage full: delete captures or increase the storage limit."));
    return;
  }
  auto output = std::make_shared<FileOutput>(m_settings->recordingsPath());
  // A compatible running producer may consume a new attachment immediately.
  // Reserve/exclude its path before publishing the output to that worker.
  const auto path = output->preparePath(game);
  m_storage->setExcludedRecordingPath(path);
  emit recordingPathPrepared(path);
  const auto id = m_engine->attach(output, game, fresh);
  if (!id) {
    m_storage->clearExcludedRecordingPath();
    emit recordingPathPrepared({});
    return;
  }
  m_output = std::move(output);
  m_id = id;
  m_game = game;
  m_failed = m_errorReported = m_quietStop = false;
  m_deferred = {};
  emit currentGameNameChanged();
  setState(RecordingState::Initializing);
}
void RecordingManager::stopRecording() {
  if (!m_id || m_state == RecordingState::Stopping) return;
  setState(RecordingState::Stopping);
  m_quietStop = false;
  emit recordingStopping(); // Attach a replacement consumer before releasing demand.
  m_engine->detach(m_id);
}
bool RecordingManager::resumeCapture() {
  if (m_id || !requiresCaptureSelection()) return false;
  start(m_game, true);
  return m_id != 0;
}
bool RecordingManager::addBookmark() {
  return isRecording() && m_output && m_output->positionMs() >= 0 && !currentFilePath().isEmpty() &&
      RecordingAnnotations::addBookmark(currentFilePath(), m_output->positionMs());
}
void RecordingManager::fileParts(bool quiet) {
  if (!m_output) return;
  for (const auto &part : m_output->takeParts()) {
    if (!part.succeeded) { m_failed = true; continue; }
    if (!part.history.isEmpty() && !RecordingAnnotations::setAudioRouting(part.path, part.history))
      qWarning() << "Could not save audio source history:" << part.path;
    m_deferred = part;
    emit recordingStopped(part.path, part.durationMs, part.bytes, quiet);
  }
}
void RecordingManager::preparePart() {
  const auto path = m_output->preparePath(m_game);
  if (m_storage) m_storage->setExcludedRecordingPath(path);
  emit recordingPathPrepared(path);
}
void RecordingManager::completeFeedback() {
  const auto part = std::exchange(m_deferred, {});
  if (!part.path.isEmpty()) emit recordingCompletionReady(part.path, part.durationMs, part.bytes);
}
} // namespace TrophyBackend
