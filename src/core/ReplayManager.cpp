/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ReplayManager.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/ReplayExporter.h"
#include "audio/AudioRoutingInfo.h"
#include "StorageManager.h"
#include "settings/SettingsManager.h"
#include "library/RecordingAnnotations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace TrophyBackend {
namespace {
// Matroska may shift all streams when a GOP contains leading audio with
// negative PTS. Its first video packet is our known GOP head; anchor that
// actual container PTS to the producer clock instead of assuming it is zero.
bool cacheClockOffset(const QString &path, qint64 headMs, double &offset, QString *error) {
  AVFormatContext *context = nullptr;
  if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0) {
    if (error) *error = QStringLiteral("Could not read DVR cache clock.");
    return false;
  }
  auto *packet = av_packet_alloc();
  bool found = false;
  if (packet && context->nb_streams && context->streams[0]->time_base.den > 0) {
    int64_t bytes = 0;
    for (int count = 0; count < 256 && av_read_frame(context, packet) >= 0; ++count) {
      bytes += std::max(packet->size, 0);
      if (bytes > 32 * 1024 * 1024) break;
      if (packet->stream_index == 0) {
        if ((packet->flags & AV_PKT_FLAG_KEY) && packet->pts != AV_NOPTS_VALUE) {
          offset = headMs / 1000.0 - packet->pts * av_q2d(context->streams[0]->time_base);
          found = true;
        }
        break;
      }
      av_packet_unref(packet);
    }
  }
  av_packet_free(&packet);
  avformat_close_input(&context);
  if (!found && error) *error = QStringLiteral("Could not identify DVR cache video origin.");
  return found;
}
}

ReplayManager::Buffer::Block::~Block() { if (!path.isEmpty()) QFile::remove(path); }
ReplayManager::Buffer::Buffer(QString cacheRoot, int seconds) : m_seconds(std::max(1, seconds)) {
  if (cacheRoot.isEmpty()) cacheRoot = QDir::tempPath() + "/trophy/replay";
  QDir().mkpath(cacheRoot);
  m_directory = std::make_shared<QTemporaryDir>(QDir(cacheRoot).filePath("packets-XXXXXX"));
  if (!m_directory->isValid()) m_error = QStringLiteral("Could not create DVR cache directory");
}
ReplayManager::Buffer::~Buffer() = default;
bool ReplayManager::Buffer::begin(const EncodedStream &stream) {
  std::lock_guard lock(m_mutex);
  m_stream = stream;
  m_current.reset();
  return m_error.isEmpty();
}
bool ReplayManager::Buffer::write(const EncodedFrame &packet) {
  std::shared_ptr<Block> outgoing, current;
  {
    std::lock_guard lock(m_mutex);
    if (!m_error.isEmpty()) return false;
    if (packet->streamIndex == 0 && packet->isKeyframe &&
        (!m_current || packet->pts + m_stream.timelineStartMs > m_current->startMs)) {
      outgoing = m_current;
      m_current = std::make_shared<Block>();
      m_current->stream = m_stream;
      m_current->directory = m_directory;
      m_current->path = m_directory->filePath(QString("gop_%1.mkv").arg(m_sequence++));
      m_current->startMs = packet->pts + m_stream.timelineStartMs;
      m_current->endMs = m_current->startMs;
      m_blocks.push_back(m_current);
    }
    current = m_current;
    if (!current) return true;
    if (packet->streamIndex == 0) {
      current->endMs = std::max(current->endMs,
          packet->pts + m_stream.timelineStartMs + std::max<int64_t>(1, packet->duration));
      m_endMs = current->endMs;
    }
    {
      std::lock_guard packetLock(current->mutex);
      current->packets.push_back(packet);
      if (packet->streamIndex == 0)
        current->videoDtsMs.push_back(packet->dts == AV_NOPTS_VALUE
            ? AV_NOPTS_VALUE : packet->dts + m_stream.timelineStartMs);
    }
    prune();
  }
  if (outgoing && !persist(outgoing)) {
    std::lock_guard lock(m_mutex);
    m_error = QStringLiteral("Failed to write DVR buffer");
    return false;
  }
  return true;
}
void ReplayManager::Buffer::prune() {
  const qint64 cutoff = m_endMs - qint64(m_seconds) * 1000;
  while (m_blocks.size() > 2 && m_blocks[1]->startMs <= cutoff)
    m_blocks.pop_front(); // Snapshots reserve their own cached files.
}
bool ReplayManager::Buffer::dump(const Block &block, const std::vector<EncodedFrame> &packets,
                               const QString &path, QString *error) {
  auto muxer = FFmpegMuxer::createShared();
  bool ok = muxer->initialize(path) && muxer->setMetadata(block.stream.metadata);
  for (size_t i = 0; ok && i < block.stream.streams.size(); ++i)
    ok = muxer->addStream(block.stream.streams[i]) == int(i);
  if (ok) ok = muxer->writeHeader();
  for (const auto &packet : packets) {
    if (!ok) break;
    ok = muxer->writePacket(*packet);
  }
  const bool finalized = muxer->finalize();
  if (!ok || !finalized) {
    if (error) *error = muxer->lastError();
    return false;
  }
  return true;
}
bool ReplayManager::Buffer::persist(const std::shared_ptr<Block> &block) {
  std::vector<EncodedFrame> packets;
  {
    std::lock_guard lock(block->mutex);
    if (block->persisted) return true;
    packets = block->packets;
  }
  if (packets.empty()) return true;
  if (!dump(*block, packets, block->path, nullptr)) return false;
  std::lock_guard lock(block->mutex);
  block->persisted = true;
  block->packets.clear();
  return true;
}
bool ReplayManager::Buffer::end() {
  std::shared_ptr<Block> current;
  {
    std::lock_guard lock(m_mutex);
    current = std::exchange(m_current, {});
  }
  const bool ok = !current || persist(current);
  if (!ok) { std::lock_guard lock(m_mutex); m_error = QStringLiteral("Failed to finalize DVR buffer"); }
  return ok;
}
bool ReplayManager::Buffer::started() const { std::lock_guard lock(m_mutex); return !m_blocks.empty(); }
QString ReplayManager::Buffer::error() const { std::lock_guard lock(m_mutex); return m_error; }
void ReplayManager::Buffer::clear() { std::lock_guard lock(m_mutex); m_current.reset(); m_blocks.clear(); }
void ReplayManager::Buffer::setAudioHistory(quint64 generation, const QJsonObject &history) {
  std::lock_guard lock(m_mutex);
  for (const auto &block : m_blocks)
    if (block->stream.generation == generation) block->history = history;
}
ReplayManager::Buffer::Snapshot ReplayManager::Buffer::snapshot(int seconds) const {
  Snapshot result;
  result.seconds = std::max(1, seconds);
  std::lock_guard lock(m_mutex);
  result.endMs = m_endMs;
  const qint64 cutoff = m_endMs - qint64(result.seconds) * 1000;
  QList<QJsonObject> histories;
  for (const auto &block : m_blocks) {
    if (block->endMs <= cutoff) continue;
    Piece piece;
    piece.block = block;
    std::lock_guard packetLock(block->mutex);
    if (!block->persisted) piece.packets = block->packets;
    piece.videoDtsMs = block->videoDtsMs;
    result.pieces.append(std::move(piece));
    histories.append(block->history);
  }
  result.history = AudioRoutingInfo::combine(histories);
  return result;
}
bool ReplayManager::Buffer::Snapshot::save(const QString &path, QString *error, double *keptSeconds) const {
  if (pieces.isEmpty()) { if (error) *error = QStringLiteral("Nothing buffered yet."); return false; }
  QTemporaryDir temporary;
  if (!temporary.isValid()) { if (error) *error = QStringLiteral("Could not prepare replay export."); return false; }
  QList<ReplaySegment> segments;
  for (int i = 0; i < pieces.size(); ++i) {
    const auto &piece = pieces[i];
    QString input = piece.block->path;
    if (!piece.packets.empty()) {
      input = temporary.filePath(QString::number(i) + ".mkv");
      if (!dump(*piece.block, piece.packets, input, error)) return false;
    }
    double offset = 0;
    if (!cacheClockOffset(input, piece.block->startMs, offset, error)) return false;
    segments.append({input, offset, piece.videoDtsMs});
  }
  return ReplayExporter::saveTail(segments, seconds, path, error, keptSeconds, endMs / 1000.0);
}
QVariantMap ReplayManager::Buffer::health() const {
  std::lock_guard lock(m_mutex);
  const double retained = m_blocks.empty() ? 0 : (m_endMs - m_blocks.front()->startMs) / 1000.0;
  return {{"bufferTargetSeconds", m_seconds}, {"bufferSeconds", std::min(double(m_seconds), retained)},
          {"retainedVideoSeconds", retained}, {"retainedSegments", int(m_blocks.size())}};
}
ReplayManager::ReplayManager(QObject *parent)
    : ReplayManager(std::make_shared<RecordingEngine>(), parent) {}
ReplayManager::ReplayManager(std::shared_ptr<RecordingEngine> engine, QObject *parent)
    : QObject(parent), m_engine(std::move(engine)) {
  connect(m_engine.get(), &RecordingEngine::captureSelectionFinished,
          this, &ReplayManager::captureSelectionFinished);
  connect(m_engine.get(), &RecordingEngine::captureRecoveryChanged, this, [this] {
    emit captureAuthorizationChanged();
    emit replayStateChanged();
  });
  connect(m_engine.get(), &RecordingEngine::stateChanged, this, &ReplayManager::replayStateChanged);
  connect(m_engine.get(), &RecordingEngine::outputStarted, this, [this](quint64 id) {
    if (id != m_id || m_stopping) return;
    m_started = true;
    emit replayStateChanged();
  });
  connect(m_engine.get(), &RecordingEngine::restoreAuthNeeded, this,
          [this](quint64 id, const QString &game, const QString &identity) {
    if (id == m_id) emit restoreAuthNeeded(game, identity);
  });
  connect(m_engine.get(), &RecordingEngine::outputError, this,
          [this](quint64 id, const QString &error) {
    if (id != m_id) return;
    m_errorReported = true;
    emit replayFailed(error);
  });
  connect(m_engine.get(), &RecordingEngine::generationEnded, this,
          [this](quint64 id, bool, bool, RecordingEngine::EndReason reason) {
    if (id == m_id && reason == RecordingEngine::EndReason::PermissionClosed && !m_stopping) {
      m_errorReported = true;
      emit replayFailed(QStringLiteral("Screen sharing ended. Select a game window to resume."));
    }
  });
  connect(m_engine.get(), &RecordingEngine::outputStopped, this,
          [this](quint64 id, bool ok, const QString &error) {
    if (id != m_id) return;
    if (!ok && !m_errorReported)
      emit replayFailed(error.isEmpty() ? QStringLiteral("Failed to finalize DVR buffer") : error);
    m_id = 0;
    m_stopping = false;
    m_started = false;
    m_buffer.reset(); // In-flight snapshots retain their own packet/file leases.
    emit replayStateChanged();
    const auto pending = std::exchange(m_pendingGame, {});
    if (!pending.isEmpty()) startDvr(pending);
  });
  m_saveTimer.setInterval(20);
  connect(&m_saveTimer, &QTimer::timeout, this, &ReplayManager::pollSave);
}
ReplayManager::~ReplayManager() {
  disconnect(m_engine.get(), nullptr, this, nullptr);
  if (m_id) m_engine->detach(m_id);
  if (m_save.valid()) m_save.get();
}
void ReplayManager::setSettingsManager(SettingsManager *settings) {
  m_settings = settings;
  m_engine->setSettingsManager(settings);
}
void ReplayManager::startDvr(const QString &game) { start(game, false); }
void ReplayManager::start(const QString &game, bool fresh) {
  if (m_paused || !m_settings || game.isEmpty() || !m_settings->instantReplayEnabled()) return;
  if (m_id) {
    if (m_stopping || m_game != game) {
      m_pendingGame = game;
      m_stopping = true;
      m_engine->detach(m_id);
    }
    return;
  }
  auto buffer = std::make_shared<Buffer>(m_settings->replayCachePath(), m_settings->clipBufferSeconds());
  const auto id = m_engine->attach(buffer, game, fresh);
  if (!id) return;
  m_id = id;
  m_buffer = std::move(buffer);
  m_game = game;
  m_started = m_stopping = m_errorReported = false;
  emit replayStateChanged();
}
bool ReplayManager::selectCaptureWindow() {
  if (m_id || !m_engine->requiresCaptureSelection()) return false;
  start(m_game, true);
  return m_id != 0;
}
void ReplayManager::stopDvr() {
  m_pendingGame.clear();
  if (m_id && !m_stopping) {
    m_stopping = true;
    emit replayStateChanged();
    m_engine->detach(m_id);
  }
}
void ReplayManager::setPaused(bool paused) {
  if (m_paused == paused) return;
  m_paused = paused;
  if (paused) stopDvr();
}
QVariantMap ReplayManager::captureHealth() const {
  if (!m_buffer) return {};
  auto result = m_engine->captureHealth();
  result.insert(m_buffer->health());
  result["mode"] = QStringLiteral("replay");
  result["waitingForWindow"] = isRecoveringCapture();
  return result;
}
void ReplayManager::saveReplay() {
  if (m_save.valid()) return;
  if (!m_buffer || !m_settings) { emit replayFailed(QStringLiteral("Nothing buffered yet.")); return; }
  const auto snapshot = m_buffer->snapshot(m_settings->clipBufferSeconds());
  if (snapshot.pieces.isEmpty()) { emit replayFailed(QStringLiteral("Nothing buffered yet.")); return; }
  if (m_storage) {
    m_storage->refresh();
    qint64 estimate = 0;
    for (const auto &piece : snapshot.pieces) {
      estimate += QFileInfo(piece.block->path).size();
      for (const auto &packet : piece.packets) estimate += qint64(packet->data.size());
    }
    if (m_storage->isOverLimit() ||
        (m_storage->maxSize() > 0 && m_storage->usedSize() + estimate > m_storage->maxSize())) {
      emit replayFailed(QStringLiteral("Storage is full. Free space or increase the limit to save clips."));
      return;
    }
  }
  const auto directory = m_settings->recordingsPath();
  if (!QDir().mkpath(directory)) { emit replayFailed(QStringLiteral("Could not create clip directory.")); return; }
  const auto path = StorageManager::newRecordingPath(directory, m_game, QStringLiteral("_clip"));
  m_save = std::async(std::launch::async, [snapshot, path] {
    SaveResult result;
    result.path = path;
    const auto temporary = path + ".part";
    result.ok = snapshot.save(temporary, &result.error, &result.seconds);
    if (result.ok) {
      result.ok = QFile::rename(temporary, path);
      if (!result.ok) result.error = QStringLiteral("Could not publish saved clip.");
    }
    if (result.ok && !snapshot.history.isEmpty())
      RecordingAnnotations::setAudioRouting(path, snapshot.history);
    if (!result.ok) QFile::remove(temporary);
    return result;
  });
  m_saveTimer.start();
}
void ReplayManager::pollSave() {
  if (!m_save.valid() || m_save.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  const auto result = m_save.get();
  m_saveTimer.stop();
  if (result.ok) emit replaySaved(result.path, result.seconds);
  else emit replayFailed(result.error);
}
} // namespace TrophyBackend
