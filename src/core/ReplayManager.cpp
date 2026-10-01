/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ReplayManager.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <thread>

#include "audio/AudioMixer.h"
#include "capture/CaptureFactory.h"
#include "core/RecordingSession.h"
#include "encoding/EncoderFactory.h"
#include "encoding/FFmpegEncoder.h"
#include "muxing/FFmpegMuxer.h"
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
}

ReplayManager::~ReplayManager() { stopDvr(); }

void ReplayManager::setSettingsManager(SettingsManager *settings) {
  m_settings = settings;
}

void ReplayManager::setState(State s) {
  if (m_state != s) {
    m_state = s;
    emit replayStateChanged();
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
    if (m_gameName == gameName)
      return; // already buffering this game
    stopDvr();
  }
  m_gameName = gameName;
  m_segments.clear();
  m_segmentIndex = 0;
  m_dvrClock.start();
  if (!beginCapture()) {
    m_gameName.clear();
    return;
  }
  setState(State::StartingCapture);
  qInfo() << "Instant replay DVR arming for" << gameName;
}

void ReplayManager::stopDvr() {
  m_rotationTimer->stop();
  const QString game = m_gameName;
  const int closedCount = m_segments.size() + (m_openSegmentPath.isEmpty() ? 0 : 1);
  if (m_session) {
    m_session->stop();
    m_session.reset();
  }
  if (m_audioMixer) {
    for (int i = 0; i < AudioMixer::kTrackCount; ++i)
      m_audioMixer->removeSource(i);
  }
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
  m_capture = CaptureFactory::createBestSource();
  if (!m_capture) {
    qWarning() << "Replay DVR: no capture source";
    return false;
  }
  connect(m_capture.get(), &IVideoCaptureSource::ready, this,
          &ReplayManager::onCaptureReady, Qt::QueuedConnection);
  connect(m_capture.get(), &IVideoCaptureSource::captureError, this,
          &ReplayManager::onCaptureError, Qt::QueuedConnection);
  connect(m_capture.get(), &IVideoCaptureSource::restoreTokenUpdated, this,
          [this](const QString &token) {
            if (m_settings && !m_gameName.isEmpty()) {
              qDebug() << "DVR: saving Wayland restore token for game:"
                       << m_gameName << "(fp:" << token.left(4) << ")";
              m_settings->setRestoreToken(m_gameName, token);
            }
          });
  connect(m_capture.get(), &IVideoCaptureSource::restoreTokenStale, this,
          [this](const QString &oldToken) {
            Q_UNUSED(oldToken)
            qWarning() << "DVR: saved restore token rejected for" << m_gameName
                       << "- re-authorizing";
            stopDvr();
            emit restoreAuthNeeded(m_gameName);
          });

  CaptureConfig cfg;
  cfg.targetFps = m_settings->fps();
  cfg.useDmaBuf = m_settings->useDmaBuf();
  cfg.windowOnly = true; // DVR is always game-targeted
  cfg.waylandRestoreToken = m_settings->getRestoreToken(m_gameName);
  if (!cfg.waylandRestoreToken.isEmpty()) {
    qDebug() << "DVR: using saved Wayland restore token for:" << m_gameName
             << "(fp:" << cfg.waylandRestoreToken.left(4) << ")";
  }
  if (!m_capture->initialize(cfg))
    return false;
  return m_capture->startCapture();
}

void ReplayManager::onCaptureReady() {
  if (m_state != State::StartingCapture)
    return;
  if (!beginSegment()) {
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

bool ReplayManager::beginSegment() {
  if (!m_capture || !m_settings)
    return false;

  m_encoder = EncoderFactory::create(m_settings->encoder());
  if (!m_encoder)
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
  if (!m_encoder->initialize(enc))
    return false;

  m_openSegmentStart = elapsedSec();
  const QString path = QString("%1/seg_%2.mkv")
                           .arg(cacheDir())
                           .arg(m_segmentIndex++, 3, 10, QChar('0'));
  m_openSegmentPath = path;
  m_muxer = std::make_unique<FFmpegMuxer>();
  if (!m_muxer->initialize(path))
    return false;

  QMap<QString, QString> meta;
  meta["title"] = m_gameName;
  meta["creation_time"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  meta["comment"] = "trophy-replay-segment";

  int audioTracks = 0;
  QStringList gameHints;
  uint32_t gamePid = 0;
  bool gameFound = false;
  int voiceTrack = -1;
  bool voiceFound = false;
  setupAudioSources(audioTracks, gameHints, gamePid, gameFound, voiceTrack,
                    voiceFound, m_micTrack);
  m_voiceTrack = voiceTrack;

  m_session = std::make_unique<RecordingSession>(
      path, m_capture.get(), m_encoder.get(), m_muxer.get(), m_audioMixer.get(),
      m_settings->separateAudioTracks(), 192, audioTracks, meta);
  connect(m_session.get(), &RecordingSession::errorOccurred, this,
          &ReplayManager::onCaptureError);
  // Wire the session's node retry (the DVR previously never re-checked:
  // voice joining mid-game and stream churn never healed until rotation).
  m_session->setGameAudioHints(gamePid, gameHints);
  m_session->setVoiceChatApp(m_settings->voiceChatApp().trimmed());
  m_session->setVoiceTrack(voiceTrack);
  m_session->setGameIsFallback(!gameFound);
  if (gameFound)
    m_session->markSourceConnected(0);
  if (voiceFound && voiceTrack >= 0)
    m_session->markSourceConnected(voiceTrack);
  // No dispatcher: segments ARE the buffer.
  if (!m_session->start())
    return false;
  return true;
}

void ReplayManager::setupAudioSources(int &trackCount,
                                       QStringList &gameHints,
                                       uint32_t &gamePid, bool &gameFound,
                                       int &voiceTrack, bool &voiceFound,
                                       int &micTrack) {
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
    const QString dev = m_settings->gameAudioDevice();
    auto desktopSrc = PipeWireAudioSource::createMonitorSource(
        dev == "default" ? QString() : dev);
    desktopSrc->setStreamName(QStringLiteral("trophy-desktop"));
    m_audioMixer->addSource(std::move(desktopSrc), 0);
    const QString micDev = m_settings->microphoneDevice();
    auto desktopMicSrc = PipeWireAudioSource::createInputSource(
        micDev == "default" ? QString() : micDev);
    desktopMicSrc->setStreamName(QStringLiteral("trophy-mic"));
    m_audioMixer->addSource(std::move(desktopMicSrc), 1);
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
  int nextContiguous = 0;
  {
    const int gameTrack = isSeparate ? 0 : nextContiguous++;
    uint32_t nodeId = 0xFFFFFFFFu;
    QString nodeName, nodeSerial;
    if (gamePid != 0) {
      nodeId =
          PipeWireAudioSource::findNodeByPid(gamePid, &nodeName, &nodeSerial);
    }
    for (const QString &hint : gameHints) {
      if (nodeId != 0xFFFFFFFFu || hint.isEmpty())
        break;
      nodeId = PipeWireAudioSource::findNodeByProcessName(hint, &nodeName,
                                                          &nodeSerial);
    }
    if (nodeId == 0xFFFFFFFFu) {
      auto fallbackSrc = PipeWireAudioSource::createMonitorSource(QString());
      fallbackSrc->setStreamName(QStringLiteral("trophy-game"));
      m_audioMixer->addSource(std::move(fallbackSrc), gameTrack);
    } else {
      auto gameSrc = PipeWireAudioSource::createApplicationSource(
          nodeId, nodeName, nodeSerial);
      gameSrc->setStreamName(QStringLiteral("trophy-game"));
      m_audioMixer->addSource(std::move(gameSrc), gameTrack);
      gameFound = true;
    }
    trackCount++;
  }
  const QString vcApp = m_settings->voiceChatApp().trimmed();
  if (!vcApp.isEmpty()) {
    QString vcName, vcSerial;
    const uint32_t vcNode = PipeWireAudioSource::findNodeByProcessName(
        vcApp, &vcName, &vcSerial);
    if (vcNode != 0xFFFFFFFFu) {
      voiceTrack = isSeparate ? 1 : nextContiguous++;
      auto voiceSrc = PipeWireAudioSource::createApplicationSource(
          vcNode, vcName, vcSerial);
      voiceSrc->setStreamName(QStringLiteral("trophy-voice"));
      m_audioMixer->addSource(std::move(voiceSrc), voiceTrack);
      trackCount++;
      voiceFound = true;
    } else if (isSeparate) {
      // Reserve the slot so the session retry has somewhere to bind when
      // the stream appears. Merged mode can't reserve (empty slot stalls
      // the mix gate).
      voiceTrack = 1;
    }
  }
  auto micSrc = PipeWireAudioSource::createInputSource(
      m_settings->microphoneDevice() == "default"
          ? QString()
          : m_settings->microphoneDevice());
  micSrc->setStreamName(QStringLiteral("trophy-mic"));
  const int micSlot = isSeparate ? 2 : nextContiguous++;
  m_audioMixer->addSource(std::move(micSrc), micSlot);
  micTrack = micSlot;
  trackCount++;
#else
  Q_UNUSED(trackCount);
#endif
}

void ReplayManager::applyMicrophoneDevice() {
  if (!m_session || !m_audioMixer || !m_settings || m_micTrack < 0)
    return;
  const QString dev = m_settings->microphoneDevice();
  auto src = PipeWireAudioSource::createInputSource(
      dev == "default" ? QString() : dev);
  src->setStreamName(QStringLiteral("trophy-mic"));
  m_audioMixer->addSource(std::move(src), m_micTrack);
  qDebug() << "DVR live-applied microphone device:" << dev;
}

void ReplayManager::applyGameAudioDevice() {
  if (!m_session || !m_audioMixer || !m_settings)
    return;
  if (m_settings->audioMode() != "desktop")
    return;
  const QString dev = m_settings->gameAudioDevice();
  auto src = PipeWireAudioSource::createMonitorSource(
      dev == "default" ? QString() : dev);
  src->setStreamName(QStringLiteral("trophy-desktop"));
  m_audioMixer->addSource(std::move(src), 0);
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

void ReplayManager::applyVoiceChatApp() {  if (!m_session || !m_audioMixer || !m_settings)
    return;
  const QString app = m_settings->voiceChatApp().trimmed();
  m_session->setVoiceChatApp(app);
  const bool isSeparate = m_settings->separateAudioTracks();
  if (app.isEmpty()) {
    if (m_voiceTrack >= 0) {
      m_audioMixer->removeSource(m_voiceTrack);
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
    m_audioMixer->removeSource(m_voiceTrack);
    m_session->forgetTrack(m_voiceTrack);
  }
  qDebug() << "DVR live-applied voice chat app:" << app;
}

void ReplayManager::finalizeSegment() {
  if (!m_session)
    return; // nothing open; capture (if any) keeps running
  // Keep the portal capture running; only the encode/mux leg stops.
  m_session->stop(true);
  m_session.reset();
  if (m_audioMixer) {
    for (int i = 0; i < AudioMixer::kTrackCount; ++i)
      m_audioMixer->removeSource(i);
  }
  m_muxer.reset();
  m_encoder.reset();
  const double now = elapsedSec();
  m_segments.push_back({m_openSegmentPath, m_openSegmentStart,
                        std::max(0.0, now - m_openSegmentStart)});
  m_openSegmentPath.clear();
}

void ReplayManager::rotateSegment() {
  if (m_state != State::Running)
    return;
  const double now = elapsedSec();
  finalizeSegment();
  pruneOldSegments(now);
  if (!beginSegment()) {
    qWarning() << "Replay DVR: rotation failed";
    stopDvr();
    emit replayFailed("Clips stopped: encoder error.");
  }
}

void ReplayManager::pruneOldSegments(double now) {
  if (!m_settings)
    return;
  const double window = std::max(1, m_settings->clipBufferSeconds());
  const QStringList dead = pruneList(m_segments, now, window);
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
  const QString ts = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
  return QString("%1/%2_%3_Replay.mkv").arg(base, safe, ts);
}

void ReplayManager::saveReplay() {
  if (m_state != State::Running || !m_settings) {
    emit replayFailed("Nothing buffered yet.");
    return;
  }
  if (m_saving)
    return;
  m_saving = true;
  m_rotationTimer->stop();

  const double now = elapsedSec();
  finalizeSegment();

  const double window = std::max(1, m_settings->clipBufferSeconds());
  QList<ReplaySegment> segs = selectForWindow(m_segments, now, window);
  // Fallback span when the remux can't report one: within the first
  // `window` seconds of buffering there is less than a full window.
  const double fallbackSec =
      segs.isEmpty()
          ? window
          : std::min(window, std::max(0.0, now - segs.front().startOffsetSec));
  const QString out = clipOutputPath();

  // Remux off the UI thread; completion hops back via queued invoke.
  // The clip is written to a .part file and atomically renamed: the
  // library watches this directory and must never see a half-written
  // file (it would generate a thumbnail for garbage).
  QPointer<ReplayManager> self(this);
  const QString tmpOut = out + ".part";
  QFile::remove(tmpOut);
  std::thread([self, segs, window, fallbackSec, out, tmpOut]() {
    QString err;
    double keptSec = 0.0;
    bool ok = ReplayClipper::saveTail(segs, window, tmpOut, &err, &keptSec);
    // Report what the file actually holds (keyframe overshoot runs up
    // to a GOP over the setting), not the aspirational setting.
    const double actualSec = keptSec > 0 ? keptSec : fallbackSec;
    if (ok && QFile::exists(out))
      QFile::remove(out);
    if (ok)
      ok = QFile::rename(tmpOut, out);
    else
      QFile::remove(tmpOut);
    if (!ok && err.isEmpty())
      err = "Could not save the clip file.";
    if (!self)
      return;
    QMetaObject::invokeMethod(
        self, [self, ok, out, err, actualSec]() {
          if (!self)
            return;
          self->m_saving = false;
          if (ok) {
            if (self->m_state == State::Running)
              self->m_rotationTimer->start();
            // Resume buffering immediately with a fresh segment.
            if (self->m_state == State::Running && !self->beginSegment()) {
              self->stopDvr();
              emit self->replayFailed("Clips stopped.");
              return;
            }
            emit self->replaySaved(out, actualSec);
          } else {
            if (self->m_state == State::Running) {
              self->m_rotationTimer->start();
              if (!self->beginSegment())
                self->stopDvr();
            }
            emit self->replayFailed(err.isEmpty() ? "Could not save replay."
                                                  : err);
          }
        },
        Qt::QueuedConnection);
  }).detach();
}

} // namespace TrophyBackend
