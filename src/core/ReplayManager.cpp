/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ReplayManager.h"
#include "detection/GameDetectionService.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <thread>

#include "audio/AudioMixer.h"
#include "capture/CaptureFactory.h"
#include "core/RecordingSession.h"
#include "core/StorageManager.h"
#include "encoding/EncoderFactory.h"
#include "encoding/FFmpegEncoder.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/TrophyMetadata.h"
#include "settings/SettingsManager.h"

#ifdef HAVE_PIPEWIRE
#include "audio/PipeWireAudioSource.h"
#endif

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace TrophyBackend {

ReplayManager::ReplayManager(QObject *parent) : QObject(parent) {
  m_rotationTimer = new QTimer(this);
  m_rotationTimer->setSingleShot(false);
  m_rotationTimer->setInterval(kSegmentSeconds * 1000);
  connect(m_rotationTimer, &QTimer::timeout, this,
          &ReplayManager::rotateSegment);
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);
  m_lookupTimer = new QTimer(this);
  m_lookupTimer->setInterval(10);
  connect(m_lookupTimer, &QTimer::timeout, this, &ReplayManager::pollLookupWorker);
  m_retirementTimer = new QTimer(this);
  m_retirementTimer->setInterval(10);
  connect(m_retirementTimer, &QTimer::timeout, this, &ReplayManager::pollRetirements);
}

ReplayManager::~ReplayManager() {
  m_shuttingDown = true;
  m_pendingGameName.clear();
  stopDvr();
  for (auto &retired : m_retiringSegments)
    retired->session->stop();
  if (m_captureStopWorker.valid()) m_captureStopWorker.get();
  if (m_saveThread.joinable()) m_saveThread.join();
  if (m_audioMixer) m_audioMixer->stopAll();
  pollRetirements();
  if (m_lookupWorker.valid()) m_lookupWorker.get();
}

void ReplayManager::setSettingsManager(SettingsManager *settings) {
  m_settings = settings;
}

void ReplayManager::setStorageManager(StorageManager *storage) {
  m_storage = storage;
}

void ReplayManager::setState(State s) {
  if (m_state != s) {
    m_state = s;
    if (!m_shuttingDown) emit replayStateChanged();
  }
}

double ReplayManager::elapsedSec() const {
  return m_dvrClock.isValid() ? m_dvrClock.elapsed() / 1000.0 : 0.0;
}

QString ReplayManager::legacyCacheDir() {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
         "/trophy/replay";
}

QString ReplayManager::cacheDir() const {
  QString base = m_settings ? m_settings->replayCachePath() : QString();
  if (base.isEmpty()) {
    // RAM-backed by default: rolling DVR segments are transient by design
    // (deleted on game close), so /tmp avoids SSD wear and stale segments
    // never survive a reboot. QDir::tempPath() honours TMPDIR.
    base = QDir::tempPath() + "/trophy/replay";
    // One-time migration from the previous default: drop its dir if it is
    // not what we use now (segments were always transient, so anything
    // left there is junk from an unclean exit).
    const QString legacy = legacyCacheDir();
    if (!legacy.isEmpty() && legacy != base && QFileInfo::exists(legacy))
      QDir(legacy).removeRecursively();
  }
  QDir().mkpath(base);
  return base;
}

// --- Pure helpers ----------------------------------------------------------

QList<ReplaySegment> ReplayManager::selectForWindow(
    const QList<Segment> &segments, double nowSec, double windowSec) {
  QList<ReplaySegment> out;
  for (const Segment &s : segments) {
    if (s.endSec() > nowSec - windowSec)
      out.push_back({s.path, s.startSec});
  }
  return out;
}

QStringList ReplayManager::pruneList(const QList<Segment> &segments,
                                     double nowSec, double windowSec) {
  QStringList dead;
  const double horizon = nowSec - windowSec - kPruneMarginSeconds;
  const int n = segments.size();
  // Oldest first, but always keep the newest kMinRetainedSegments.
  for (int i = 0; i + kMinRetainedSegments < n; ++i) {
    if (segments[i].endSec() < horizon)
      dead.push_back(segments[i].path);
    else
      break;
  }
  return dead;
}

// --- Lifecycle -------------------------------------------------------------

void ReplayManager::startDvr(const QString &gameName) {
  if (!m_settings || gameName.isEmpty())
    return;
  if (!m_settings->instantReplayEnabled())
    return;
  if (m_paused)
    return;
  if (m_state != State::Idle) {
    if (m_state == State::Stopping) {
      m_pendingGameName = gameName;
      return;
    }
    if (m_gameName == gameName)
      return; // already buffering this game
    stopDvr();
    if (m_state == State::Idle) startDvr(gameName);
    else m_pendingGameName = gameName;
    return;
  }
  m_gameName = gameName;
  ++m_captureGeneration;
  m_captureReadyForPipeline = false;
  m_segments.clear();
  m_segmentIndex = 0;
  m_dvrClock.start();
  if (!beginCapture()) {
    stopDvr();
    return;
  }
  setState(State::StartingCapture);
  qInfo() << "Instant replay DVR arming for" << gameName;
}

void ReplayManager::stopDvr() {
  m_pendingGameName.clear(); // Explicit stop supersedes any deferred restart.
  if (m_state == State::Stopping)
    return;
  ++m_captureGeneration;
  m_rotationTimer->stop();
  m_captureReadyForPipeline = false;
  // A clip worker may still be reading finalized segments. Retire it before
  // clearing those files or destroying the manager that owns its callback.
  m_saveWaitingForRetirement = false;
  setState(State::Stopping);
  if (m_session) {
    auto retired = std::make_unique<RetiringSegment>();
    retired->session = std::move(m_session);
    retired->encoder = std::move(m_encoder);
    retired->muxer = std::move(m_muxer);
    retired->segment = {m_openSegmentPath, m_openSegmentStart,
                       std::max(0.0, elapsedSec() - m_openSegmentStart)};
    retired->session->requestStop();
    m_retiringSegments.push_back(std::move(retired));
    m_openSegmentPath.clear();
  } else if (m_capture) {
    m_capture->prepareStopCapture();
    m_captureStopWorker = std::async(std::launch::async,
        [capture = m_capture.get()] { capture->finishStopCapture(); });
  }
  if (m_audioMixer) m_audioMixer->stopSourcesAsync();
  m_audioSetupKey.clear();
  // Drop the node-lookup cache and orphan any refresh worker in flight
  // (its delivery checks the generation and discards itself).
  m_lookupCacheValid = false;
  ++m_lookupGeneration;
  m_retirementTimer->start();
  pollRetirements();
}

void ReplayManager::completeDvrStop() {
  const QString game = m_gameName;
  const int closedCount = m_segments.size() + (m_openSegmentPath.isEmpty() ? 0 : 1);
  m_saveInputPaths.clear();
  m_saving = false;
  m_muxer.reset();
  m_encoder.reset();
  m_capture.reset();
  // The open segment was finalized to disk by session->stop() above but
  // never enlisted in m_segments — remove it here or one stale seg file
  // leaks into the cache dir per game session.
  if (!m_openSegmentPath.isEmpty()) {
    QFile::remove(m_openSegmentPath);
    m_openSegmentPath.clear();
  }
  m_openSegmentStart = 0.0;
  clearSegments();
  m_gameName.clear();
  setState(State::Idle);
  if (!game.isEmpty())
    qInfo() << "Instant replay DVR stopped for" << game << "("
             << closedCount << "cached segment(s) cleared)";
  const QString pending = std::exchange(m_pendingGameName, {});
  if (!m_shuttingDown && m_state == State::Idle && !pending.isEmpty())
    startDvr(pending);
}

void ReplayManager::setPaused(bool paused) {
  if (m_paused == paused)
    return;
  m_paused = paused;
  if (paused) {
    stopDvr(); // clears the buffer; resumes fresh afterwards
  }
}

void ReplayManager::clearSegments() {
  for (const Segment &s : m_segments)
    QFile::remove(s.path);
  m_segments.clear();
}

bool ReplayManager::beginCapture() {
  m_captureIdentity = m_gameDetector ? m_gameDetector->captureKeyForGame(m_gameName) : m_gameName;
  m_capture = m_captureFactory ? m_captureFactory() : CaptureFactory::createBestSource();
  if (!m_capture) {
    qWarning() << "Replay DVR: no capture source";
    return false;
  }
  connectCaptureSignals();

  CaptureConfig cfg;
  cfg.targetFps = m_settings->fps();
  cfg.useDmaBuf = m_settings->useDmaBuf();
  cfg.windowOnly = true; // DVR is always game-targeted
  cfg.waylandRestoreToken = m_settings->getRestoreTokenForIdentity(m_captureIdentity, m_gameName);
  if (!cfg.waylandRestoreToken.isEmpty()) {
    qDebug() << "DVR: using saved Wayland restore token for:" << m_gameName
             << "(fp:" << cfg.waylandRestoreToken.left(4) << ")";
  }
  if (!m_capture->initialize(cfg))
    return false;
  return m_capture->startCapture();
}

void ReplayManager::connectCaptureSignals() {
  const QPointer<IVideoCaptureSource> owner(m_capture.get());
  const uint64_t generation = m_captureGeneration;
  const QString tokenKey = m_captureIdentity;
  connect(m_capture.get(), &IVideoCaptureSource::ready, this,
          [this, owner, generation] {
            if (generation == m_captureGeneration && owner && owner == m_capture.get())
              onCaptureReady();
          }, Qt::QueuedConnection);
  connect(m_capture.get(), &IVideoCaptureSource::captureError, this,
          [this, owner, generation](const QString &error) {
            if (generation == m_captureGeneration && owner && owner == m_capture.get())
              onCaptureError(error);
          }, Qt::QueuedConnection);
  connect(m_capture.get(), &IVideoCaptureSource::restoreTokenUpdated, this,
          [this, owner, generation, tokenKey](const QString &token) {
            if (generation == m_captureGeneration && owner && owner == m_capture.get() && m_settings &&
                !m_gameName.isEmpty()) {
              qDebug() << "DVR: saving Wayland restore token for game:"
                       << m_gameName << "(fp:" << token.left(4) << ")";
              m_settings->setRestoreTokenForIdentity(tokenKey, m_gameName, token);
            }
          });
  connect(m_capture.get(), &IVideoCaptureSource::restoreTokenStale, this,
          [this, owner, generation](const QString &token) {
            if (generation == m_captureGeneration && owner && owner == m_capture.get())
              onRestoreTokenStale(token);
          }, Qt::QueuedConnection);

}

void ReplayManager::onRestoreTokenStale(const QString &oldToken) {
  Q_UNUSED(oldToken)
  // Copy before stopDvr(): it clears m_gameName, and the re-auth prompt
  // needs the identity (RecordingManager follows the same pattern).
  const QString game = m_gameName;
  const QString identity = m_captureIdentity;
  qWarning() << "DVR: saved restore token rejected for" << game
             << "- re-authorizing";
  stopDvr();
  emit restoreAuthNeeded(game, identity);
}

void ReplayManager::onCaptureReady() {
  if (m_state != State::StartingCapture)
    return;
  if (!m_captureReadyForPipeline)
    emit captureSelectionFinished();
  m_captureReadyForPipeline = true;
  if (!m_lookupCacheValid || m_lookupAudioKey != audioSetupKey()) {
    refreshLookupCacheAsync();
    return;
  }
  if (!(m_segmentBuilder ? m_segmentBuilder() : switchSegment())) {
    qWarning() << "Replay DVR: segment failed to start";
    stopDvr();
    emit replayFailed("Clips could not start buffering.");
    return;
  }
  setState(State::Running);
  m_rotationTimer->start();
  qInfo() << "Instant replay DVR running for" << m_gameName;
}

void ReplayManager::onCaptureError(const QString &error) {
  qWarning() << "Replay DVR capture error:" << error;
  const bool wasRunning = m_state == State::Running;
  stopDvr();
  if (wasRunning)
    emit replayFailed("Clips stopped: capture error.");
}

QString ReplayManager::audioSetupKey() const {
  if (!m_settings)
    return {};
  // Desktop mode always merges, so the separate flag is normalized out:
  // flipping it must not spuriously rebuild the running sources.
  const bool effectiveSeparate = m_settings->separateAudioTracks() &&
                                 m_settings->audioMode() != "desktop";
  return m_settings->audioMode() + "|" + m_settings->gameAudioDevice() +
         "|" + m_settings->microphoneDevice() + "|" +
         m_settings->voiceChatApp().trimmed() + "|" +
         (effectiveSeparate ? "sep" : "mix");
}

bool ReplayManager::switchSegment() {
  if (!m_capture || !m_settings)
    return false;

  // Build the replacement leg while the current session (if any) keeps
  // recording: encoder init alone costs hundreds of ms, and paying it
  // up-front is what makes the handoff gapless.
  auto newEncoder = EncoderFactory::create(m_settings->encoder());
  if (!newEncoder)
    return false;

  EncoderConfig enc;
  QSize native = m_capture->nativeResolution();
  enc.width = native.width();
  enc.height = native.height();
  if (enc.width % 2 != 0)
    enc.width--;
  if (enc.height % 2 != 0)
    enc.height--;
  if (enc.width <= 0 || enc.height <= 0)
    return false;
  enc.fps = m_settings->fps();
  enc.bitrate = m_settings->videoBitrate();
  // DVR-only 1s GOP: clip cuts land on keyframes, so a shorter interval
  // keeps saved clips within ~1s of the buffer setting (30.0–31.0s reads
  // as 0:30). Full recordings keep the 2s default — smaller files, and
  // nothing trims them mid-stream. Slightly larger segment files is the
  // price; they live in cache and rotate away.
  enc.keyframeInterval = 1;
  enc.maxBitrate = m_settings->maxVideoBitrate();
  enc.useVBR = m_settings->useVBR();
  enc.preset = m_settings->encoderPreset();
  enc.inputPixelFormat = AV_PIX_FMT_BGRA;
  if (!newEncoder->initialize(enc))
    return false;

  const QString path = QString("%1/seg_%2.mkv")
                           .arg(cacheDir())
                           .arg(m_segmentIndex++, 3, 10, QChar('0'));
  auto newMuxer = std::make_unique<FFmpegMuxer>();
  if (!newMuxer->initialize(path)) {
    QFile::remove(path);
    return false;
  }

  QMap<QString, QString> meta;
  meta["title"] = m_gameName;
  meta["creation_time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  meta["comment"] = "trophy-replay-segment";
  // Segment start moment (+ recorder version): the clipper inherits these
  // via av_dict_copy, then overwrites them with its own save moment.
  stampTrophyVersionDateTime(meta);
  if (newEncoder)
    stampTrophyEncoder(meta, newEncoder->name());

  // Audio node lookup; the running PipeWire sources are recreated only
  // when the audio configuration changed (or on the very first segment).
  // Otherwise the live streams carry straight over the handoff and only
  // the encoders rotate — no per-switch renegotiation gap.
  const QString key = audioSetupKey();
  const bool rebuildSources = !m_session || key != m_audioSetupKey;
  if (rebuildSources && m_audioMixer) {
    for (int i = 0; i < AudioMixer::kTrackCount; ++i)
      m_audioMixer->removeSourceAsync(i);
  }
  int audioTracks = 0;
  QStringList gameHints;
  uint32_t gamePid = 0;
  bool gameFound = false;
  int voiceTrack = -1;
  bool voiceFound = false;
  setupAudioSources(audioTracks, gameHints, gamePid, gameFound, voiceTrack,
                    voiceFound, m_micTrack, rebuildSources);
  m_voiceTrack = voiceTrack;
  if (rebuildSources)
    m_audioSetupKey = key;

  auto newSession = std::make_unique<RecordingSession>(
      path, m_capture.get(), newEncoder.get(), newMuxer.get(),
      // Desktop mode always records one merged track (system + mic):
      // the saved separate flag must not produce multi-stream files.
      m_audioMixer.get(),
      m_settings->separateAudioTracks() &&
          m_settings->audioMode() != "desktop",
      192, audioTracks, meta);
  connect(newSession.get(), &RecordingSession::errorOccurred, this,
          [this, owner = QPointer<RecordingSession>(newSession.get())]
          (const QString &error) {
            // Start failures are handled by switchSegment's caller. A late
            // error from a retired leg must not tear down its successor.
            if (owner && owner == m_session.get())
              onCaptureError(error);
          }, Qt::QueuedConnection);
  // Wire the session's node retry (the DVR previously never re-checked:
  // voice joining mid-game and stream churn never healed until rotation).
  newSession->setGameAudioHints(gamePid, gameHints);
  newSession->setVoiceChatApp(m_settings->voiceChatApp().trimmed());
  newSession->setVoiceTrack(voiceTrack);
  newSession->setGameIsFallback(!gameFound);
  if (gameFound)
    newSession->markSourceConnected(0);
  if (voiceFound && voiceTrack >= 0)
    newSession->markSourceConnected(voiceTrack);
  if (!rebuildSources && m_session)
    newSession->inheritAudioRouting(*m_session);
  // No dispatcher: segments ARE the buffer.
  if (m_session)
    m_session->retireAudioLookups();
  if (!newSession->start()) {
    QFile::remove(path);
    return false;
  }

  // The successor owns the live routing now (capture + mixer callbacks
  // swapped atomically inside start). Retire the old leg with a handoff
  // stop: it drains its queues, flushes its encoders and finalizes its
  // file, but leaves capture, mixer sources and callbacks live.
  const double switchSec = elapsedSec();
  const bool retired = retireOpenSegment(switchSec);
  m_encoder = std::move(newEncoder);
  m_muxer = std::move(newMuxer);
  m_session = std::move(newSession);
  m_openSegmentStart = switchSec;
  m_openSegmentPath = path;
  // Publish the successor even on failure: the caller's stopDvr() must
  // drain the leg that now owns capture/mixer routing before destroying it.
  return retired;
}

bool ReplayManager::retireOpenSegment(double switchSec) {
  if (!m_session)
    return true;
  auto retired = std::make_unique<RetiringSegment>();
  retired->session = std::move(m_session);
  retired->encoder = std::move(m_encoder);
  retired->muxer = std::move(m_muxer);
  retired->segment = {m_openSegmentPath, m_openSegmentStart,
                     std::max(0.0, switchSec - m_openSegmentStart)};
  retired->session->requestStop(true, true);
  m_retiringSegments.push_back(std::move(retired));
  m_retirementTimer->start();
  m_openSegmentPath.clear();
  return true;
}

void ReplayManager::pollRetirements() {
  if (m_saveThread.joinable() && m_saveDone &&
      m_saveDone->load(std::memory_order_acquire)) {
    m_saveThread.join();
    m_saveInputPaths.clear();
    m_saving = false;
    auto result = std::move(m_saveResult);
    if (result && !m_shuttingDown) {
      if (result->ok) emit replaySaved(result->path, result->seconds);
      else emit replayFailed(result->error.isEmpty() ? "Could not save replay." : result->error);
    }
  }
  bool failed = false;
  for (auto it = m_retiringSegments.begin(); it != m_retiringSegments.end();) {
    auto &retired = **it;
    if (retired.session->stopPending()) { ++it; continue; }
    const bool succeeded = retired.session->outputSucceeded();
    if (succeeded && m_state != State::Stopping) {
      m_segments.push_back(retired.segment);
      std::sort(m_segments.begin(), m_segments.end(), [](const auto &a, const auto &b) {
        return a.startSec < b.startSec;
      });
    } else {
      QFile::remove(retired.segment.path);
      if (!succeeded && m_state == State::Running) failed = true;
    }
    it = m_retiringSegments.erase(it); // QObject destruction stays on owner.
  }
  if (failed) {
    stopDvr();
    emit replayFailed("Clips stopped: recording pipeline or output failed.");
    return;
  }
  if (m_state == State::Stopping) {
    if (!m_retiringSegments.empty()) return;
    if (m_captureStopWorker.valid()) {
      if (m_captureStopWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
      m_captureStopWorker.get();
    }
    for (int track = 0; track < AudioMixer::kTrackCount; ++track)
      if (m_audioMixer->sourceOperationPending(track)) return;
    if (m_saveThread.joinable()) {
      if (m_saveDone && !m_saveDone->load(std::memory_order_acquire)) return;
      m_saveThread.join();
    }
    m_retirementTimer->stop();
    completeDvrStop();
    return;
  }
  if (m_saveWaitingForRetirement) {
    const bool selectedPending = std::any_of(
        m_retiringSegments.begin(), m_retiringSegments.end(), [&](const auto &retired) {
      return m_saveInputPaths.contains(retired->segment.path);
    });
    if (!selectedPending) {
      m_saveWaitingForRetirement = false;
      beginReplayRemux(m_saveRequestNow);
    }
  }
  if (m_retiringSegments.empty() && !m_saveThread.joinable() &&
      !m_saveWaitingForRetirement)
    m_retirementTimer->stop();
}

ReplayManager::DvrNodeLookup
ReplayManager::lookupDvrNodes(uint32_t gamePid, const QStringList &gameHints,
                              const QString &voiceApp) {
  DvrNodeLookup out;
  if (gamePid != 0) {
    out.gameNodeId = PipeWireAudioSource::findNodeByPid(
        gamePid, &out.gameNodeName, &out.gameNodeSerial);
  }
  for (const QString &hint : gameHints) {
    if (out.gameNodeId != 0xFFFFFFFFu || hint.isEmpty())
      break;
    out.gameNodeId = PipeWireAudioSource::findNodeByProcessName(
        hint, &out.gameNodeName, &out.gameNodeSerial);
  }
  out.gameFound = (out.gameNodeId != 0xFFFFFFFFu);
  if (!voiceApp.isEmpty()) {
    out.voiceNodeId = PipeWireAudioSource::findNodeByProcessName(
        voiceApp, &out.voiceNodeName, &out.voiceNodeSerial);
    out.voiceFound = (out.voiceNodeId != 0xFFFFFFFFu);
  }
  return out;
}

void ReplayManager::refreshLookupCacheAsync() {
  if (m_lookupWorker.valid() || !m_settings)
    return;
  // Snapshot inputs on the GUI thread; the worker runs pure lookups only.
  const uint32_t pid = m_capture ? m_capture->sourcePid() : 0;
  QStringList hints;
  if (m_capture) {
    const QString hint = m_capture->sourceProcessName();
    if (!hint.isEmpty())
      hints << hint;
  }
  if (!m_gameName.isEmpty() && !hints.contains(m_gameName, Qt::CaseInsensitive))
    hints << m_gameName;
  const QString vcApp = m_settings->voiceChatApp().trimmed();
  const quint64 generation = ++m_lookupGeneration;
  m_lookupInflight = true;
  m_workerLookupGeneration = generation;
  m_workerAudioKey = audioSetupKey();
  m_workerLookupCapture = m_capture.get();
  const bool desktop = m_settings->audioMode() == "desktop";
  m_lookupWorker = std::async(std::launch::async,
      [lookup = m_nodeLookup, pid, hints, vcApp, desktop] {
    return desktop ? DvrNodeLookup{} : lookup(pid, hints, vcApp);
  });
  m_lookupTimer->start();
}

void ReplayManager::pollLookupWorker() {
  if (!m_lookupWorker.valid()) { m_lookupTimer->stop(); return; }
  if (m_lookupWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  const auto result = m_lookupWorker.get();
  m_lookupTimer->stop();
  m_lookupInflight = false;
  if (m_workerAudioKey == audioSetupKey() && m_workerLookupCapture == m_capture.get()) {
    applyLookupCacheResult(m_workerLookupGeneration, result);
    if (m_workerLookupGeneration == m_lookupGeneration)
      m_lookupAudioKey = m_workerAudioKey;
  }
  if (m_state == State::StartingCapture && m_captureReadyForPipeline)
    onCaptureReady();
}

void ReplayManager::applyLookupCacheResult(quint64 generation,
                                          const DvrNodeLookup &fresh) {
  m_lookupInflight = false;
  // Drop stale results: a newer refresh, a stopDvr (which bumps the
  // generation), or a fresh synchronous lookup superseded us.
  if (generation != m_lookupGeneration)
    return;
  m_lookupCache = fresh;
  m_lookupCacheValid = true;
}

void ReplayManager::setupAudioSources(int &trackCount,
                                       QStringList &gameHints,
                                       uint32_t &gamePid, bool &gameFound,
                                       int &voiceTrack, bool &voiceFound,
                                       int &micTrack, bool createSources) {
  const int retainedMicTrack = micTrack;
  trackCount = 0;
  gameHints.clear();
  gamePid = 0;
  gameFound = false;
  voiceTrack = -1;
  voiceFound = false;
  micTrack = -1;
  if (!m_audioMixer || !m_settings)
    return;
#ifdef HAVE_PIPEWIRE
  const bool isSeparate = m_settings->separateAudioTracks();
  const QString mode = m_settings->audioMode();
  if (mode == "desktop") {
    // Lookup-only refresh skips source creation: the running monitor
    // streams carry over the handoff untouched.
    if (createSources) {
      const QString dev = m_settings->gameAudioDevice();
      auto desktopSrc = PipeWireAudioSource::createMonitorSource(
          dev == "default" ? QString() : dev, true);
      desktopSrc->setStreamName(QStringLiteral("trophy-desktop"));
      m_audioMixer->addSourceAsync(std::move(desktopSrc), 0);
      const QString micDev = m_settings->microphoneDevice();
      auto desktopMicSrc = PipeWireAudioSource::createInputSource(
          micDev == "default" ? QString() : micDev, true);
      desktopMicSrc->setStreamName(QStringLiteral("trophy-mic"));
      m_audioMixer->addSourceAsync(std::move(desktopMicSrc), 1);
    }
    trackCount = 2;
    micTrack = 1;
    return;
  }
  // Game mode: game node (PID first, name hints, monitor fallback), voice,
  // mic. Proton nodes expose only the exe basename, so the DVR game name
  // (detector display name) is a first-class hint.
  gamePid = m_capture ? m_capture->sourcePid() : 0;
  if (m_capture) {
    const QString hint = m_capture->sourceProcessName();
    if (!hint.isEmpty())
      gameHints << hint;
  }
  if (!m_gameName.isEmpty() &&
      !gameHints.contains(m_gameName, Qt::CaseInsensitive))
    gameHints << m_gameName;
  const QString vcApp = m_settings->voiceChatApp().trimmed();
  if (!createSources && m_session) {
    // Availability is not slot identity. A merged voice source may have joined
    // slot 2 after the mic was installed in slot 1; recomputing from the cache
    // would swap them (or drop voice from the mix when its node disappears).
    const auto routing = m_session->audioRoutingState();
    trackCount = routing.activeTrackCount;
    voiceTrack = routing.voiceTrack;
    micTrack = retainedMicTrack;
    gameFound = routing.connected[0] && !routing.gameIsFallback;
    voiceFound = voiceTrack >= 0 && voiceTrack < AudioMixer::kTrackCount &&
                 routing.connected[voiceTrack];
    return;
  }
  // Startup waits for an owned immutable lookup. A settings-driven rebuild
  // uses matching cached results or fallback routing plus a background refresh;
  // no registry walk runs from this owner-thread source publication path.
  const bool useCache = m_lookupCacheValid && m_lookupAudioKey == audioSetupKey();
  const DvrNodeLookup lookup =
      useCache ? m_lookupCache : DvrNodeLookup{};
  if (!useCache) {
    refreshLookupCacheAsync();
  }
  int nextContiguous = 0;
  {
    const int gameTrack = isSeparate ? 0 : nextContiguous++;
    const uint32_t nodeId = lookup.gameNodeId;
    if (nodeId == 0xFFFFFFFFu) {
      if (createSources) {
        auto fallbackSrc = PipeWireAudioSource::createMonitorSource(QString(), true);
        fallbackSrc->setStreamName(QStringLiteral("trophy-game"));
        m_audioMixer->addSourceAsync(std::move(fallbackSrc), gameTrack);
      }
    } else {
      if (createSources) {
        auto gameSrc = PipeWireAudioSource::createApplicationSource(
            nodeId, lookup.gameNodeName, lookup.gameNodeSerial);
        gameSrc->setStreamName(QStringLiteral("trophy-game"));
        m_audioMixer->addSourceAsync(std::move(gameSrc), gameTrack);
      }
      gameFound = true;
    }
    trackCount++;
  }
  if (!vcApp.isEmpty()) {
    if (lookup.voiceNodeId != 0xFFFFFFFFu && gameFound) {
      voiceTrack = isSeparate ? 1 : nextContiguous++;
      if (createSources) {
        auto voiceSrc = PipeWireAudioSource::createApplicationSource(
            lookup.voiceNodeId, lookup.voiceNodeName,
            lookup.voiceNodeSerial);
        voiceSrc->setStreamName(QStringLiteral("trophy-voice"));
        m_audioMixer->addSourceAsync(std::move(voiceSrc), voiceTrack);
      }
      trackCount++;
      voiceFound = true;
    } else {
      // Suppressed while track 0 is full-mix fallback (see
      // RecordingManager): the monitor already carries voice. Reserve
      // the named slot in separate mode so the session retry can bind
      // it once the game track upgrades; merged mode adopts a free slot
      // only when voice appears, preserving its mic assignment.
      if (!gameFound && lookup.voiceNodeId != 0xFFFFFFFFu) {
        qDebug() << "DVR voice chat capture suppressed while game audio "
                    "is full-mix fallback (voice is already in the mix):"
                 << vcApp;
      }
      if (isSeparate) {
        voiceTrack = 1;
      }
    }
  }
  if (createSources) {
    auto micSrc = PipeWireAudioSource::createInputSource(
        m_settings->microphoneDevice() == "default"
            ? QString()
            : m_settings->microphoneDevice(), true);
    micSrc->setStreamName(QStringLiteral("trophy-mic"));
    const int micSlot = isSeparate ? 2 : nextContiguous++;
    m_audioMixer->addSourceAsync(std::move(micSrc), micSlot);
    micTrack = micSlot;
  } else {
    micTrack = isSeparate ? 2 : nextContiguous++;
  }
  trackCount++;
#else
  Q_UNUSED(trackCount);
#endif
}

void ReplayManager::applyMicrophoneDevice() {
  if (m_state != State::Running) return;
  if (!m_session || !m_audioMixer || !m_settings || m_micTrack < 0)
    return;
  const QString dev = m_settings->microphoneDevice();
  auto src = PipeWireAudioSource::createInputSource(
      dev == "default" ? QString() : dev, true);
  src->setStreamName(QStringLiteral("trophy-mic"));
  m_audioMixer->addSourceAsync(std::move(src), m_micTrack);
  qDebug() << "DVR live-applied microphone device:" << dev;
}

void ReplayManager::applyGameAudioDevice() {
  if (m_state != State::Running) return;
  if (!m_session || !m_audioMixer || !m_settings)
    return;
  if (m_settings->audioMode() != "desktop")
    return;
  const QString dev = m_settings->gameAudioDevice();
  auto src = PipeWireAudioSource::createMonitorSource(
      dev == "default" ? QString() : dev, true);
  src->setStreamName(QStringLiteral("trophy-desktop"));
  m_audioMixer->addSourceAsync(std::move(src), 0);
  qDebug() << "DVR live-applied system audio device:" << dev;
}

void ReplayManager::setTrackMuted(int trackIndex, bool muted) {
  if (m_audioMixer)
    m_audioMixer->setTrackMuted(trackIndex, muted);
}

bool ReplayManager::isTrackMuted(int trackIndex) const {
  if (m_audioMixer)
    return m_audioMixer->isTrackMuted(trackIndex);
  return false;
}

float ReplayManager::trackLevel(int trackIndex) const {
  if (m_audioMixer)
    return m_audioMixer->getTrackLevel(trackIndex);
  return 0.0f;
}

void ReplayManager::setSemanticTrackMuted(int role, bool muted) {
  if (m_audioMixer) m_audioMixer->setSemanticTrackMuted(role, muted);
}

bool ReplayManager::isSemanticTrackMuted(int role) const {
  return m_audioMixer && m_audioMixer->isSemanticTrackMuted(role);
}

float ReplayManager::semanticTrackLevel(int role) const {
  return m_state == State::Running && m_audioMixer
             ? m_audioMixer->semanticTrackLevel(role) : 0.0f;
}

void ReplayManager::applyVoiceChatApp() {
  if (m_state != State::Running) return;
  if (!m_session || !m_audioMixer || !m_settings)
    return;
  // The session can adopt voice after initial setup; the manager's original
  // slot is then stale until rotation. Live changes must target the actual leg.
  m_voiceTrack = m_session->audioRoutingState().voiceTrack;
  const QString app = m_settings->voiceChatApp().trimmed();
  m_session->setVoiceChatApp(app);
  const bool isSeparate = m_settings->separateAudioTracks();
  if (app.isEmpty()) {
    if (m_voiceTrack >= 0) {
      m_audioMixer->removeSourceAsync(m_voiceTrack);
      m_session->forgetTrack(m_voiceTrack);
    }
    qDebug() << "DVR voice chat disabled mid-segment";
    return;
  }
  if (m_voiceTrack < 0 && isSeparate) {
    m_voiceTrack = 1;
    m_session->setVoiceTrack(1);
  }
  if (m_voiceTrack >= 0) {
    m_audioMixer->removeSourceAsync(m_voiceTrack);
    m_session->forgetTrack(m_voiceTrack);
  }
  qDebug() << "DVR live-applied voice chat app:" << app;
}

void ReplayManager::rotateSegment() {
  if (m_state != State::Running)
    return;
  const double now = elapsedSec();
  if (!switchSegment()) {
    qWarning() << "Replay DVR: rotation failed";
    stopDvr();
    emit replayFailed("Clips stopped: recording pipeline or output failed.");
    return;
  }
  pruneOldSegments(now);
}

void ReplayManager::pruneOldSegments(double now) {
  if (!m_settings)
    return;
  const double window = std::max(1, m_settings->clipBufferSeconds());
  QStringList dead = pruneList(m_segments, now, window);
  // Rotation may overlap an arbitrarily slow remux. Keep every selected
  // input until its worker settles, not merely until it opens the file.
  for (const QString &input : m_saveInputPaths)
    dead.removeAll(input);
  for (const QString &p : dead)
    QFile::remove(p);
  if (dead.isEmpty())
    return;
  const QSet<QString> deadSet(dead.begin(), dead.end());
  m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(),
                                  [&](const Segment &s) {
                                    return deadSet.contains(s.path);
                                  }),
                   m_segments.end());
}

QString ReplayManager::clipOutputPath() const {
  QString base = m_settings ? m_settings->recordingsPath() : QString();
  QString safe = m_gameName;
  safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
  safe.replace(QRegularExpression("[_-]{2,}"), "_");
  safe = safe.trimmed().replace(QRegularExpression("^[_-]+|[_-]+$"), "");
  if (safe.isEmpty())
    safe = "Game";
  const QString ts =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
  return QString("%1/%2_%3_Replay.mkv").arg(base, safe, ts);
}

void ReplayManager::saveReplay() {
  if (m_state != State::Running || !m_settings) {
    emit replayFailed("Nothing buffered yet.");
    return;
  }
  if (m_saving)
    return;
  // Storage pre-flight: clips must never push usage past the limit.
  // Check-only (no enforce/auto-delete): a clip refusing is cheaper and
  // safer than the recording path's cleanup-then-record. The failure
  // surfaces through the existing replayFailed -> "Couldn't Save Clip"
  // notice in Application.
  if (m_storage) {
    m_storage->refresh();
    if (m_storage->isOverLimit()) {
      emit replayFailed("Storage is full. Free space or enable auto-cleanup "
                        "to save clips.");
      return;
    }
  }
  m_saving = true;

  // Gapless switch first: the new segment is already recording before the
  // remux thread starts, so nothing is lost during the remux (and a
  // rotation firing mid-remux is harmless — it only touches live legs).
  // The remux itself only ever reads finalized files.
  const double now = elapsedSec();
  if (!switchSegment()) {
    m_saving = false;
    stopDvr();
    emit replayFailed("Clips stopped: recording pipeline or output failed.");
    return;
  }

  m_saveRequestNow = now;
  m_saveRequestWindow = std::max(1, m_settings->clipBufferSeconds());
  m_saveRequestOutput = clipOutputPath();
  m_saveInputPaths.clear();
  for (const auto &segment : selectForWindow(m_segments, now, m_saveRequestWindow))
    m_saveInputPaths << segment.path;
  for (const auto &retired : m_retiringSegments)
    if (retired->segment.endSec() > now - m_saveRequestWindow)
      m_saveInputPaths << retired->segment.path;
  m_saveWaitingForRetirement = true;
  m_retirementTimer->start();
  pollRetirements();
}

void ReplayManager::beginReplayRemux(double now) {
  if (m_state != State::Running || !m_settings) {
    m_saving = false;
    m_saveInputPaths.clear();
    return;
  }
  const double window = m_saveRequestWindow;
  QList<ReplaySegment> segs = selectForWindow(m_segments, now, window);
  segs.erase(std::remove_if(segs.begin(), segs.end(), [&](const auto &segment) {
    return !m_saveInputPaths.contains(segment.path);
  }), segs.end()); // A later rotation must not move this request's clip tail.
  // Would-exceed guard: the remux stream-copies the selected segments, so
  // their on-disk total bounds the clip size (segments live in the cache
  // dir, uncounted in library usage — the clip adds ~that much to it).
  if (m_storage && m_storage->maxSize() > 0) {
    qint64 estimate = 0;
    for (const ReplaySegment &s : segs)
      estimate += QFileInfo(s.path).size();
    if (m_storage->usedSize() + estimate > m_storage->maxSize()) {
      m_saving = false;
      m_saveInputPaths.clear();
      emit replayFailed("Storage is full. Free space or enable auto-cleanup "
                        "to save clips.");
      return;
    }
  }
  // Fallback span when the remux can't report one: within the first
  // `window` seconds of buffering there is less than a full window.
  const double fallbackSec =
      segs.isEmpty()
          ? window
          : std::min(window, std::max(0.0, now - segs.front().startOffsetSec));
  const QString out = m_saveRequestOutput;

  // Remux off the UI thread; completion hops back via queued invoke.
  // The clip is written to a .part file and atomically renamed: the
  // library watches this directory and must never see a half-written
  // file (it would generate a thumbnail for garbage).
  const QString tmpOut = out + ".part";
  if (m_saveThread.joinable())
    return; // Never reuse a previous worker's input reservation.
  m_saveInputPaths.clear();
  for (const ReplaySegment &segment : segs)
    m_saveInputPaths.push_back(segment.path);
  m_saveDone = std::make_shared<std::atomic<bool>>(false);
  m_saveResult = std::make_shared<ReplaySaveResult>();
  m_saveThread = std::thread([segs, window, fallbackSec, out, tmpOut,
                              done = m_saveDone, result = m_saveResult]() {
    struct Completion {
      std::shared_ptr<std::atomic<bool>> done;
      ~Completion() { done->store(true, std::memory_order_release); }
    } completion{done};
    QString err;
    double keptSec = 0.0;
    bool ok = ReplayClipper::saveTail(segs, window, tmpOut, &err, &keptSec);
    // Report what the file actually holds (keyframe overshoot runs up
    // to a GOP over the setting), not the aspirational setting.
    const double actualSec = keptSec > 0 ? keptSec : fallbackSec;
    if (ok)
      ok = QFile::rename(tmpOut, out);
    if (!ok)
      QFile::remove(tmpOut);
    if (!ok && err.isEmpty())
      err = "Could not save the clip file.";
    result->ok = ok;
    result->path = out;
    result->error = err;
    result->seconds = actualSec;
  });
  m_retirementTimer->start();
}

} // namespace TrophyBackend
