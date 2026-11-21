/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "RecordingEngine.h"
#include "RecordingSession.h"
#include "encoding/IEncoder.h"
#include "audio/AudioMixer.h"
#include "detection/GameDetectionService.h"
#include "settings/SettingsManager.h"
#include <QPointer>
#include <QElapsedTimer>
#include <QTimer>
#include <atomic>
#include <future>
#include <map>
#include <mutex>

namespace TrophyBackend {
struct RecordingEngine::Shared {
  struct Slot {
    quint64 id = 0, intent = 0;
    std::shared_ptr<EncodedOutput> output;
    std::atomic<bool> active{true};
    bool releasing = false; // Owner-thread demand; remains a drain target until cut.
    std::mutex mutex;
    quint64 generation = 0;
    bool begun = false, ready = false;
    std::atomic<bool> failed{false};
    QString error;
  };
  std::mutex mutex;
  std::map<quint64, std::shared_ptr<Slot>> outputs;
  EncodedStream format;
  std::atomic<qint64> timelineMs{0};
};
struct RecordingEngine::Private {
  struct Job {
    std::shared_ptr<Shared::Slot> slot;
    std::future<bool> future;
    bool succeeded = true;
    QString reason;
  };
  std::vector<std::unique_ptr<Job>> jobs;
  std::unique_ptr<AudioMixer> ownedMixer;
  quint64 nextId = 1, nextIntent = 1, intent = 0, nextGeneration = 1;
  QString game, fingerprint, pendingGame;
  quint64 pendingIntent = 0;
  bool pending = false, freshSelection = false, shuttingDown = false;
  QString inputError;
  QElapsedTimer configurationAge;
  bool restartRequested = false, restartReady = false;
  QTimer timer;
};

namespace {
QString inputFingerprint(const SettingsManager *settings) {
  if (!settings) return {};
  return QString("%1|%2|%3|%4|%5|%6|%7|%8|%9")
      .arg(settings->encoder()).arg(settings->fps()).arg(settings->videoBitrate())
      .arg(settings->useVBR()).arg(settings->encoderPreset()).arg(settings->videoResolution())
      .arg(settings->maxVideoBitrate()).arg(settings->audioMode())
      .arg(settings->audioMode() != "desktop" && settings->separateAudioTracks());
}
}

// RecordingSession's encoded endpoint. This object writes no files itself.
class EnginePacketMuxer final : public IMuxer {
public:
  EnginePacketMuxer(RecordingEngine *engine, quint64 generation, quint64 intent,
                   QString game, QVariantMap identity)
      : m_engine(engine), m_shared(engine->m_shared), m_intent(intent) {
    m_format.generation = generation;
    m_format.timelineStartMs = m_shared->timelineMs.load();
    m_format.game = std::move(game);
    m_format.identity = std::move(identity);
  }
  bool initialize(const QString &) override { return true; }
  int addStream(const StreamInfo &stream) override {
    m_format.streams.push_back(stream);
    return int(m_format.streams.size() - 1);
  }
  bool setMetadata(const QMap<QString, QString> &metadata) override {
    m_format.metadata = metadata;
    return true;
  }
  bool updateStreamExtraData(int stream, const std::vector<uint8_t> &data) override {
    if (stream < 0 || size_t(stream) >= m_format.streams.size()) return false;
    m_format.streams[stream].extraData = data;
    return true;
  }
  bool writeHeader() override {
    std::lock_guard lock(m_shared->mutex);
    m_shared->format = m_format;
    return !m_format.streams.empty();
  }
  bool writePacket(const EncodedPacket &packet) override {
    const auto frame = std::make_shared<const EncodedPacket>(packet);
    if (packet.streamIndex == 0) {
      const qint64 end = m_format.timelineStartMs + packet.pts + std::max<int64_t>(1, packet.duration);
      m_shared->timelineMs.store(std::max(m_shared->timelineMs.load(), end));
    }
    m_bytes.fetch_add(qint64(packet.data.size()));
    for (const auto &slot : targets()) {
      std::lock_guard lock(slot->mutex);
      if (!slot->active.load()) continue;
      bool ok = true;
      if (slot->generation != m_format.generation) {
        slot->generation = m_format.generation;
        slot->ready = false;
        slot->begun = true;
        ok = slot->output->begin(m_format);
      }
      if (ok) ok = slot->output->write(frame);
      if (!ok) {
        slot->failed = true;
        slot->active.store(false);
        slot->error = slot->output->error();
        const auto id = slot->id;
        const auto error = slot->error.isEmpty() ? QStringLiteral("Recording output failed") : slot->error;
        if (m_engine) QMetaObject::invokeMethod(m_engine, [owner = m_engine, id, error] {
          if (owner) owner->consumerFailed(id, error);
        }, Qt::QueuedConnection);
      } else if (!slot->ready && slot->output->started()) {
        slot->ready = true;
        const auto id = slot->id;
        if (m_engine) QMetaObject::invokeMethod(m_engine, [owner = m_engine, id] {
          if (owner) emit owner->outputStarted(id);
        }, Qt::QueuedConnection);
      }
    }
    // A consumer's I/O failure is isolated. The owner detaches that consumer;
    // surviving outputs keep receiving the same uninterrupted encoded stream.
    return true;
  }
  bool finalize() override {
    if (m_finalized.exchange(true)) return true;
    for (const auto &slot : targets()) {
      std::lock_guard lock(slot->mutex);
      if (slot->begun && slot->generation == m_format.generation) {
        if (!slot->output->end()) {
          slot->failed = true;
          slot->error = slot->output->error();
        }
        slot->begun = false;
      }
    }
    return true;
  }
  QString outputPath() const override { return QStringLiteral("encoded-packet-stream"); }
  int64_t bytesWritten() const override { return m_bytes.load(); }
  QString formatName() const override { return QStringLiteral("encoded-packets"); }
private:
  std::vector<std::shared_ptr<RecordingEngine::Shared::Slot>> targets() const {
    std::vector<std::shared_ptr<RecordingEngine::Shared::Slot>> result;
    std::lock_guard lock(m_shared->mutex);
    for (const auto &[id, slot] : m_shared->outputs)
      if (slot->intent == m_intent && slot->active.load()) result.push_back(slot);
    return result;
  }
  QPointer<RecordingEngine> m_engine;
  std::shared_ptr<RecordingEngine::Shared> m_shared;
  EncodedStream m_format;
  quint64 m_intent;
  std::atomic<qint64> m_bytes{0};
  std::atomic<bool> m_finalized{false};
};

RecordingEngine::RecordingEngine(QObject *parent)
    : QObject(parent), m_shared(std::make_shared<Shared>()), m_private(std::make_unique<Private>()) {
  initializeProducer();
  m_private->ownedMixer = std::make_unique<AudioMixer>(48000, 2);
  configureCaptureAudio(m_private->ownedMixer.get());
  m_muxerFactory = [this] {
    return std::make_unique<EnginePacketMuxer>(this, m_private->nextGeneration++,
        m_private->intent, currentGameName(), m_detectedGameInfo);
  };
  connect(this, &RecordingEngine::nativeStateChanged, this, [this] {
    emit stateChanged();
    QTimer::singleShot(0, this, &RecordingEngine::reconcile);
  });
  connect(this, &RecordingEngine::captureRecoveryChanged, this, [this] {
    emit stateChanged();
    QTimer::singleShot(0, this, &RecordingEngine::reconcile);
  });
  connect(this, &RecordingEngine::nativeError, this, [this](const QString &error) {
    m_private->inputError = error;
  });
  connect(this, &RecordingEngine::nativeStarted, this, [this] {
    m_private->fingerprint = inputFingerprint(m_settings);
    m_private->configurationAge.start();
    emit stateChanged();
  });
  connect(this, &RecordingEngine::nativeProgress, this, [this] {
    const auto history = m_session ? m_session->audioHistory() : QJsonObject{};
    std::vector<std::shared_ptr<Shared::Slot>> outputs;
    { std::lock_guard lock(m_shared->mutex);
      for (const auto &[id, slot] : m_shared->outputs) if (slot->active.load()) outputs.push_back(slot); }
    quint64 generation;
    { std::lock_guard lock(m_shared->mutex); generation = m_shared->format.generation; }
    for (const auto &slot : outputs) slot->output->setAudioHistory(generation, history);
    emit progressUpdated();
    if (isRecording() && m_private->configurationAge.elapsed() >= kReconfigurationSeconds * 1000 &&
        m_private->fingerprint != inputFingerprint(m_settings) &&
        m_captureLifecycle.instanceIsCurrent() && hasDemand()) {
      m_private->restartRequested = true;
      stopRecording();
    }
  });
  m_private->timer.setInterval(10);
  connect(&m_private->timer, &QTimer::timeout, this, &RecordingEngine::pollOutputs);
}
RecordingEngine::~RecordingEngine() {
  m_private->shuttingDown = true;
  m_private->timer.stop();
  disconnect(this, nullptr, this, nullptr);
  shutdownProducer(); // Drain native callbacks before releasing packet consumers.
  for (auto &job : m_private->jobs) if (job->future.valid()) job->future.get();
  m_private->jobs.clear();
  std::lock_guard lock(m_shared->mutex);
  m_shared->outputs.clear();
}
void RecordingEngine::setSettingsManager(SettingsManager *settings) {
  if (m_settings == settings) return;
  configureCaptureSettings(settings);
}
void RecordingEngine::setGameDetectionService(GameDetectionService *detector) {
  if (m_gameDetector == detector) return;
  configureCaptureDetection(detector);
}
void RecordingEngine::setAudioMixer(AudioMixer *mixer) {
  if (!mixer || hasDemand()) return;
  configureCaptureAudio(mixer);
}
quint64 RecordingEngine::attach(std::shared_ptr<EncodedOutput> output, const QString &game, bool fresh) {
  if (!output || !m_settings || (requiresCaptureSelection() && !fresh)) return 0;
  auto slot = std::make_shared<Shared::Slot>();
  slot->id = m_private->nextId++;
  slot->output = std::move(output);
  if (canReuseCapture(game) && !fresh) {
    slot->intent = m_private->intent;
  } else if (m_private->pending && m_private->pendingGame == game && !fresh) {
    slot->intent = m_private->pendingIntent;
  } else {
    m_private->pending = true;
    m_private->pendingGame = game;
    m_private->pendingIntent = m_private->nextIntent++;
    m_private->freshSelection = fresh;
    slot->intent = m_private->pendingIntent;
  }
  { std::lock_guard lock(m_shared->mutex); m_shared->outputs.emplace(slot->id, slot); }
  if (slot->intent == m_private->intent && m_videoEncoder)
    m_videoEncoder->requestKeyframe();
  QTimer::singleShot(0, this, &RecordingEngine::reconcile);
  return slot->id;
}
void RecordingEngine::detach(quint64 id) {
  {
    std::lock_guard lock(m_shared->mutex);
    const auto found = m_shared->outputs.find(id);
    if (found == m_shared->outputs.end()) return;
    found->second->releasing = true;
  }
  QTimer::singleShot(0, this, &RecordingEngine::reconcile);
}
void RecordingEngine::closeOutput(quint64 id, bool succeeded, const QString &reason) {
  std::shared_ptr<Shared::Slot> slot;
  { std::lock_guard lock(m_shared->mutex);
    const auto found = m_shared->outputs.find(id);
    if (found == m_shared->outputs.end()) return;
    slot = found->second;
    if (!slot->active.exchange(false) && !slot->failed) return;
    slot->failed = false; // Only one close job per output.
    m_shared->outputs.erase(found); }
  auto job = std::make_unique<Private::Job>();
  job->slot = slot;
  job->succeeded = succeeded;
  job->reason = reason;
  quint64 generation;
  { std::lock_guard lock(m_shared->mutex); generation = m_shared->format.generation; }
  const auto history = m_session ? m_session->audioHistory() : QJsonObject{};
  job->future = std::async(std::launch::async, [slot, generation, history] {
    std::lock_guard lock(slot->mutex);
    const bool ok = !slot->begun || slot->output->end();
    slot->output->setAudioHistory(generation, history);
    slot->begun = false;
    return ok;
  });
  m_private->jobs.push_back(std::move(job));
  m_private->timer.start();
  QTimer::singleShot(0, this, &RecordingEngine::reconcile);
}
void RecordingEngine::consumerFailed(quint64 id, const QString &reason) {
  emit outputError(id, reason);
  closeOutput(id, false, reason);
}
void RecordingEngine::pollOutputs() {
  for (auto it = m_private->jobs.begin(); it != m_private->jobs.end();) {
    auto &job = **it;
    if (job.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
    const bool ok = job.future.get() && job.succeeded;
    const auto id = job.slot->id;
    const auto error = job.reason.isEmpty() ? job.slot->output->error() : job.reason;
    it = m_private->jobs.erase(it);
    emit outputStopped(id, ok, error);
  }
  if (m_private->jobs.empty()) m_private->timer.stop();
  reconcile();
}
void RecordingEngine::generationFinished(bool succeeded, bool continuing, const QJsonObject &history) {
  const bool restart = std::exchange(m_private->restartRequested, false) &&
      succeeded && m_private->inputError.isEmpty() && hasDemand() && !m_private->pending &&
      m_captureLifecycle.instanceIsCurrent();
  m_private->restartReady = restart;
  continuing |= restart;
  const auto reason = continuing ? EndReason::Handoff :
      !succeeded || !m_private->inputError.isEmpty() ? EndReason::Failed :
      captureResumeRequired() ? EndReason::PermissionClosed : EndReason::Stopped;
  std::vector<std::shared_ptr<Shared::Slot>> outputs;
  { std::lock_guard lock(m_shared->mutex);
    for (const auto &[id, slot] : m_shared->outputs)
      if (slot->intent == m_private->intent && slot->active.load()) outputs.push_back(slot); }
  quint64 generation;
  { std::lock_guard lock(m_shared->mutex); generation = m_shared->format.generation; }
  for (const auto &slot : outputs) {
    bool ok;
    QString error;
    {
      std::lock_guard lock(slot->mutex);
      if (!history.isEmpty()) slot->output->setAudioHistory(generation, history);
      ok = succeeded && !slot->failed.load();
      error = !slot->error.isEmpty() ? slot->error : m_private->inputError;
    }
    const bool retain = continuing && ok && !slot->releasing;
    emit generationEnded(slot->id, ok, retain, ok ? reason : EndReason::Failed);
    if (!retain) {
      if (!ok || reason == EndReason::Failed) emit outputError(slot->id,
          error.isEmpty() ? QStringLiteral("Recording pipeline failed") : error);
      closeOutput(slot->id, ok && reason != EndReason::Failed, error);
    }
  }
  if (!continuing) m_private->intent = 0;
  QTimer::singleShot(0, this, &RecordingEngine::reconcile);
}
void RecordingEngine::reconcile() {
  if (m_private->shuttingDown) return;
  std::vector<quint64> obsolete, releasing;
  bool currentDemand = false, pendingDemand = false;
  { std::lock_guard lock(m_shared->mutex);
    for (const auto &[id, slot] : m_shared->outputs) {
      if (!slot->active.load()) continue;
      if (slot->releasing) { releasing.push_back(id); continue; }
      if (slot->intent == m_private->intent) currentDemand = true;
      if (m_private->pending && slot->intent == m_private->pendingIntent) pendingDemand = true;
      if (m_private->pending && slot->intent != m_private->pendingIntent &&
          slot->intent != m_private->intent) obsolete.push_back(id);
    } }
  for (auto id : obsolete) closeOutput(id);
  // A surviving consumer keeps the producer live; cut only the departing
  // output. With no successor, drain the producer before closing its last sink.
  if (currentDemand || m_state == RecordingState::Idle)
    for (auto id : releasing) closeOutput(id);
  if (m_private->pending && !pendingDemand) m_private->pending = false;
  if (m_private->pending && pendingDemand) {
    if (m_state == RecordingState::Idle && !isRecoveringCapture()) {
      m_private->intent = m_private->pendingIntent;
      m_private->game = m_private->pendingGame;
      m_private->pending = false;
      m_private->inputError.clear();
      m_private->fingerprint = inputFingerprint(m_settings);
      m_forceCaptureSelectionOnce = m_private->freshSelection;
      startRecording(m_private->game);
      if (m_state == RecordingState::Idle) {
        generationFinished(false, false, {});
      }
    } else if (m_private->pendingIntent != m_private->intent) {
      if (m_state != RecordingState::Stopping)
        stopRecording();
    }
  } else if (!currentDemand) {
    if (m_state == RecordingState::Initializing ||
        m_state == RecordingState::Recording ||
        isRecoveringCapture()) stopRecording();
    if (m_state == RecordingState::Idle)
      m_private->intent = 0;
  } else if (currentDemand && m_state == RecordingState::Idle &&
             !isRecoveringCapture()) {
    if (std::exchange(m_private->restartReady, false) &&
        m_captureLifecycle.instanceIsCurrent()) {
      startRecording(m_private->game);
      if (m_state == RecordingState::Idle)
        generationFinished(false, false, {});
    } else {
      generationFinished(m_private->inputError.isEmpty(), false, {});
    }
  }
}
bool RecordingEngine::canReuseCapture(const QString &game) const {
  const bool producing = isRecording() || m_state == RecordingState::Initializing;
  if (!producing || m_private->pending || requiresCaptureSelection() || m_private->game != game ||
      m_private->fingerprint != inputFingerprint(m_settings)) return false;
  if (!game.isEmpty() && m_gameDetector) {
    const auto pin = m_detectedGameInfo.value("detectedGameInstance").toString();
    return !pin.isEmpty() && m_gameDetector->instanceAlive(pin) &&
        m_gameDetector->captureIdentityInfo(game).value("detectedGameInstance").toString() == pin;
  }
  return true;
}
RecordingEngine::State RecordingEngine::state() const {
  if (isRecoveringCapture()) return State::Recovering;
  switch (m_state) {
  case RecordingState::Initializing: return State::Starting;
  case RecordingState::Recording: return State::Running;
  case RecordingState::Stopping: return State::Stopping;
  default: return State::Idle;
  }
}
bool RecordingEngine::hasDemand() const {
  std::lock_guard lock(m_shared->mutex);
  for (const auto &[id, slot] : m_shared->outputs)
    if (slot->active.load() && !slot->releasing) return true;
  return false;
}
bool RecordingEngine::captureResumeRequired() const { return m_captureResumeRequired; }
QString RecordingEngine::currentGame() const { return m_currentGameName; }
uint RecordingEngine::capturedSteamAppId() const { return m_detectedGameInfo.value("detectedSteamAppId").toUInt(); }
std::shared_ptr<CaptureLifecycle::Authorization> RecordingEngine::captureAuthorization() const {
  return m_captureLifecycle.authorization();
}
void RecordingEngine::setCaptureAuthorization(std::shared_ptr<CaptureLifecycle::Authorization> value) {
  m_captureLifecycle.setAuthorization(std::move(value));
}
void RecordingEngine::setSemanticTrackMuted(int track, bool muted) { m_audioMixer->setSemanticTrackMuted(track, muted); }
bool RecordingEngine::isSemanticTrackMuted(int track) const { return m_audioMixer->isSemanticTrackMuted(track); }
float RecordingEngine::semanticTrackLevel(int track) const { return m_audioMixer->semanticTrackLevel(track); }
void RecordingEngine::setTrackMuted(int track, bool muted) { m_audioMixer->setTrackMuted(track, muted); }
bool RecordingEngine::isTrackMuted(int track) const { return m_audioMixer->isTrackMuted(track); }
float RecordingEngine::trackLevel(int track) const { return m_audioMixer->getTrackLevel(track); }
void RecordingEngine::notifyRestoreAuth(const QString &game, const QString &identity) {
  std::vector<quint64> ids;
  { std::lock_guard lock(m_shared->mutex);
    for (const auto &[id, slot] : m_shared->outputs)
      if (slot->intent == m_private->intent && slot->active.load()) ids.push_back(id); }
  for (auto id : ids) emit restoreAuthNeeded(id, game, identity);
}
} // namespace TrophyBackend
