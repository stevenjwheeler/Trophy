/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingEngine.h"
#include "RecordingSession.h"
#include "audio/AudioMixer.h"
#include <QDateTime>
#include <QMap>
#include <qlogging.h>
#include "capture/IVideoCaptureSource.h"
#include "detection/GameDetectionService.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"
#include "muxing/TrophyMetadata.h"
#include "settings/SettingsManager.h"
#include "capture/CaptureFactory.h"
#include "encoding/EncoderFactory.h"
#include "detection/ProcessGames.h"
#ifdef HAVE_PIPEWIRE
#include "audio/PipeWireAudioSource.h"
#endif
extern "C" {
#include <libavutil/pixfmt.h>
}

#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace TrophyBackend {
bool RecordingEngine::startCapture(bool windowOnly, const QString &restoreToken) {
  if (!m_captureSource || !m_settings) return false;
  CaptureConfig config;
  config.targetFps = m_settings->fps();
  config.useDmaBuf = m_settings->useDmaBuf();
  config.windowOnly = windowOnly;
  config.waylandRestoreToken = restoreToken;
  return m_captureSource->initialize(config) && m_captureSource->startCapture();
}

std::unique_ptr<IEncoder> RecordingEngine::createEncoder() {
  if (!m_captureSource || !m_settings) return {};
  auto encoder = EncoderFactory::create(m_settings->encoder());
  if (!encoder) return {};
  EncoderConfig config;
  const auto resolution = m_settings->videoResolution().toLower().split('x');
  QSize canvas;
  if (resolution.size() == 2) {
    canvas = QSize(resolution[0].toInt(), resolution[1].toInt());
  } else {
    canvas = m_captureSource->nativeResolution();
    m_settings->setAutoResolutionWidth(canvas.width());
    m_settings->setAutoResolutionHeight(canvas.height());
  }
  config.width = canvas.width() & ~1;
  config.height = canvas.height() & ~1;
  if (config.width <= 0 || config.height <= 0) return {};
  config.fps = m_settings->fps();
  config.bitrate = m_settings->videoBitrate();
  config.maxBitrate = m_settings->maxVideoBitrate();
  config.useVBR = m_settings->useVBR();
  config.preset = m_settings->encoderPreset();
  config.keyframeInterval = 1;
  config.inputPixelFormat = AV_PIX_FMT_BGRA;
  if (!encoder->initialize(config)) return {};
  return encoder;
}

RecordingEngine::AudioPreparation RecordingEngine::snapshotAudio(
    const SettingsManager &settings, IVideoCaptureSource *capture,
    const QString &game, const QVariantMap &identity, bool manual) {
  AudioPreparation input;
  input.mode = settings.audioMode();
  input.gameDevice = settings.gameAudioDevice();
  input.micDevice = settings.microphoneDevice();
  input.voiceApp = settings.voiceChatApp().trimmed();
  input.separate = settings.separateAudioTracks();
  if (manual) {
    input.pid = capture ? capture->sourcePid() : 0;
    if (input.pid) input.hints = {"trophy-instance:" + ProcessGames::pinProcess(input.pid)};
  } else {
    input.pid = identity.value("detectedGamePid").toUInt();
    input.hints = {"trophy-instance:" + identity.value("detectedGameInstance").toString()};
    if (!game.isEmpty()) input.hints << game;
  }
  return input;
}

RecordingEngine::AudioRouting RecordingEngine::configureAudio(
    AudioMixer *mixer, const AudioPreparation &input) {
  AudioRouting routing;
#ifdef HAVE_PIPEWIRE
  if (!mixer) return routing;
  const auto device = [](const QString &id) { return id == "default" ? QString() : id; };
  if (input.mode == "desktop") {
    routing.trackCount = 2;
    routing.micTrack = 1;
    auto system = PipeWireAudioSource::createMonitorSource(device(input.gameDevice), true);
    system->setStreamName(QStringLiteral("trophy-desktop"));
    mixer->addSourceAsync(std::move(system), 0);
    auto mic = PipeWireAudioSource::createInputSource(device(input.micDevice), true);
    mic->setStreamName(QStringLiteral("trophy-mic"));
    mixer->addSourceAsync(std::move(mic), 1);
    return routing;
  }
  routing.gameFound = input.gameNode != 0xffffffffu;
  routing.gamePending = !routing.gameFound;
  routing.voiceFound = !input.voiceApp.isEmpty() && !routing.gamePending &&
                       input.voiceNode != 0xffffffffu;
  routing.voiceTrack = !input.voiceApp.isEmpty() && (input.separate || routing.voiceFound) ? 1 : -1;
  routing.micTrack = input.separate ? 2 : (routing.voiceFound ? 2 : 1);
  routing.trackCount = input.separate ? 3 : (routing.voiceFound ? 3 : 2);
  if (routing.gameFound) {
    auto game = PipeWireAudioSource::createApplicationSource(
        input.gameNode, input.gameNodeName, input.gameSerial, {},
        PipeWireAudioSource::ownerIdentity(input.hints));
    game->setStreamName(QStringLiteral("trophy-game"));
    mixer->addSourceAsync(std::move(game), 0);
  } else {
    auto fallback = PipeWireAudioSource::createMonitorSource({}, true);
    fallback->setStreamName(QStringLiteral("trophy-game"));
    mixer->addSourceAsync(std::move(fallback), 0);
  }
  if (routing.voiceFound) {
    auto voice = PipeWireAudioSource::createApplicationSource(
        input.voiceNode, input.voiceNodeName, input.voiceSerial, input.voiceApp);
    voice->setStreamName(QStringLiteral("trophy-voice"));
    mixer->addSourceAsync(std::move(voice), routing.voiceTrack);
  }
  auto mic = PipeWireAudioSource::createInputSource(device(input.micDevice), true);
  mic->setStreamName(QStringLiteral("trophy-mic"));
  mixer->addSourceAsync(std::move(mic), routing.micTrack);
#else
  Q_UNUSED(mixer)
  Q_UNUSED(input)
#endif
  return routing;
}

QVariantMap RecordingEngine::captureHealth() const {
  if (!isRecording() || !m_session) return {};
  auto result = m_session->captureHealth();
  result.insert(m_detectedGameInfo);
  return result;
}

void RecordingEngine::initializeProducer() {
  // Progress update timer
  m_progressTimer = new QTimer(this);
  m_progressTimer->setInterval(1000); // Update every second
  connect(m_progressTimer, &QTimer::timeout, this,
          &RecordingEngine::updateProgress);
  m_operationTimer = new QTimer(this);
  m_operationTimer->setInterval(10);
  connect(m_operationTimer, &QTimer::timeout, this, &RecordingEngine::pollOperations);
}

void RecordingEngine::shutdownProducer() {
  // Dependencies must outlive this manager. Final shutdown may join native
  // work, but must not emit signals that re-arm capture from a half-destroyed
  // Application (its unique_ptr has already been reset by then).
  m_shuttingDown = true;
  m_progressTimer->stop();
  m_operationTimer->stop();
  if (m_audioPreparationWorker.valid())
    m_audioPreparationWorker.get();
  if (m_stopFinalizationWorker.valid())
    m_stopFinalizationSucceeded = m_stopFinalizationWorker.get();
  if (m_session) m_session->stop();
  teardownPipeline();
}

void RecordingEngine::configureCaptureSettings(SettingsManager *settings) {
  m_settings = settings;
  m_captureLifecycle.configure(m_gameDetector, m_settings);
}

void RecordingEngine::configureCaptureAudio(AudioMixer *mixer) {
  m_audioMixer = mixer;
}


void RecordingEngine::configureCaptureDetection(GameDetectionService *detector) {
  if (m_gameDetector)
    disconnect(m_gameDetector, nullptr, this, nullptr);
  m_gameDetector = detector;
  m_captureLifecycle.configure(m_gameDetector, m_settings);
  if (detector) {
    const auto refresh = [this] {
      const auto previousFences = m_captureLifecycle.selectionFences();
      QSet<QString> liveInstances;
      for (const auto &game : m_gameDetector->games())
        liveInstances.insert(game.id);
      m_captureLifecycle.selectionFences().intersect(liveInstances);
      if (m_captureSelectionRequired && !interruptionIdentityIsCurrent())
        clearCaptureInterruption();
      else if (!m_captureLifecycle.selectionFences().isEmpty() ||
               previousFences != m_captureLifecycle.selectionFences())
        emit captureRecoveryChanged();
    };
    connect(detector, &GameDetectionService::currentGameChanged, this, refresh);
    connect(detector, &GameDetectionService::instancesChanged, this, refresh);
    connect(detector, &GameDetectionService::blacklistChanged, this, refresh);
  }
}

bool RecordingEngine::isGenericSourceLabel(const QString &label) {
  const QString lower = label.trimmed().toLower();
  // Compositor/shell labels are never a real capture identity: the portal
  // often reports the compositor itself ("kwin_wayland", "gnome-shell", ...)
  // instead of the picked window, so treat them as generic and fall back.
  if (lower.isEmpty() || lower == "unknown game" ||
      lower.contains("kwin") || lower.contains("wayland") ||
      lower.contains("gnome") || lower.contains("mutter") ||
      lower.contains("sway") || lower.contains("hypr")) {
    return true;
  }
  // Bare portal app-id with no title behind it (e.g. steam_app_1422450):
  // evidence-free, so the detector guess stays preferable.
  static const QRegularExpression appIdRe("^steam_app_\\d+$");
  return appIdRe.match(lower).hasMatch();
}

bool RecordingEngine::sourceLabelsAgree(const QString &a,
                                         const QString &b) {
  const QString al = a.trimmed().toLower();
  const QString bl = b.trimmed().toLower();
  if (al.isEmpty() || bl.isEmpty()) {
    return false;
  }
  return al.contains(bl) || bl.contains(al);
}

void RecordingEngine::startRecording(const QString &gameName) {
  if (m_state != RecordingState::Idle) {
    qWarning() << "Cannot start recording: not in Idle state";
    return;
  }
  if (!gameName.isEmpty() && requiresCaptureSelection() && !m_forceCaptureSelectionOnce) {
    qInfo() << "Screen sharing ended; explicit window selection is required to resume";
    return; // Automatic starts cannot reacquire an ambiguously closed session.
  }
  m_explicitCaptureSelectionAttempt = std::exchange(m_forceCaptureSelectionOnce, false);
  m_portalClosurePending = false;
  cancelCaptureRecovery(QStringLiteral("starting capture"));

  qDebug() << "startRecording called. gameName:" << gameName;

  if (!m_settings) {
    emit nativeError("Recording engine not properly initialized");
    return;
  }

  m_isManualRecording = gameName.isEmpty();
  m_captureGameName = gameName;
  m_pendingCaptureError.clear();
  m_stopNativeSettled = false;
  m_captureReadyForPipeline = false;
  m_cancelInitializing = false;
  m_pendingRestoreAuthorization.clear();
  m_pendingRestoreIdentity.clear();
  m_detectedGameInfo.clear();
  ++m_pipelineGeneration;
  m_recordingFailed = false;

  m_currentGameName =
      gameName.isEmpty() ? QStringLiteral("Manual Recording") : gameName;

  setState(RecordingState::Initializing);

  // Logical generation name only. Packet consumers choose their own storage.
  m_streamLabel = streamLabel();

  // 1. Create and initialize capture source
  m_captureSource = m_captureFactory ? m_captureFactory() : CaptureFactory::createBestSource();

  // Distinguish manual vs automatic for restore token usage
  bool isManual = gameName.isEmpty();

  if (!m_captureSource) {
    emit nativeError("Failed to create capture source");
    m_streamLabel.clear();
    setState(RecordingState::Idle);
    return;
  }

  // Connect ready signal to finish the pipeline
  const QPointer<IVideoCaptureSource> attemptSource(m_captureSource.get());
  const uint64_t attemptGeneration = m_pipelineGeneration;
  const QString tokenKey = m_gameDetector
                               ? m_gameDetector->captureKeyForGame(m_currentGameName)
                               : m_currentGameName;
  if (!m_isManualRecording && m_gameDetector) {
    m_detectedGameInfo = m_gameDetector->captureIdentityInfo(m_currentGameName);
  }
  m_captureLifecycle.pin(m_currentGameName, m_detectedGameInfo);
  m_captureLifecycle.bindSource(m_captureSource.get(), m_currentGameName, tokenKey,
          m_isManualRecording,
          [this, attemptSource, attemptGeneration] {
            return attemptGeneration == m_pipelineGeneration && attemptSource &&
                   attemptSource == m_captureSource.get();
          }, [this] { finishPipelineAndStart(); },
          [this](const QString &error) { onCaptureError(error); },
          [this, tokenKey](const QString &oldToken) {
            if (m_state != RecordingState::Initializing)
              return;
            Q_UNUSED(oldToken)
            const QString gameName = m_currentGameName;
            qWarning() << "Saved restore token rejected for"
                       << gameName << "- re-authorizing";
            m_pendingRestoreAuthorization = gameName;
            m_pendingRestoreIdentity = tokenKey;
            stopRecording();
          });

  QString restoreToken;

  // Use restore token only for automatic recordings, always prompt for manual
  if (!isManual && !m_explicitCaptureSelectionAttempt) {
    restoreToken =
        m_settings->getRestoreTokenForIdentity(tokenKey, m_currentGameName);
    if (!restoreToken.isEmpty()) {
      qDebug() << "Using saved Wayland restore token for:" << m_currentGameName
               << "(fp:" << restoreToken.left(4) << ")";
    }
  }

  if (!startCapture(!isManual, restoreToken)) {
    emit nativeError("Failed to initialize or start capture source");
    stopRecording();
    return;
  }

  // We wait for the 'ready' signal to initialize the rest of the pipeline
  qDebug() << "Waiting for capture source to be ready...";
}

void RecordingEngine::finishPipelineAndStart() {
  if (m_state != RecordingState::Initializing) {
    return;
  }
  if (m_explicitCaptureSelectionAttempt) {
    m_explicitCaptureSelectionAttempt = false;
    m_captureLifecycle.selectionFences().remove(
        m_detectedGameInfo.value("detectedGameInstance").toString());
    clearCaptureInterruption();
  }
  if (!m_captureReadyForPipeline)
    emit captureSelectionFinished();
  m_captureReadyForPipeline = true;

  qDebug() << "Capture source ready, finishing pipeline...";

  if (m_captureSource) {
    QString label = m_captureSource->sourceLabel();

    // Wayland Fallback: If XDG Portal/PipeWire hides the application name,
    // query the GameDetectionService for the foreground active game.
    if (label.isEmpty() || label.toLower().contains("kwin") ||
        label.toLower().contains("wayland") ||
        label.toLower().contains("gnome") ||
        label.toLower().contains("mutter") ||
        label.toLower().contains("sway") ||
        label.toLower().contains("hypr") || label == "Unknown Game") {
      if (m_gameDetector && m_gameDetector->isGameRunning()) {
        label = m_gameDetector->currentGameName();
        qDebug() << "RecordingManager: Using GameDetectionService name fallback:"
                << label;
      } else {
        qDebug()
            << "RecordingManager: Wayland captured generic node, but no "
               "game is currently detected. Falling back to generic system "
               "audio routing.";
      }
    }

    if (m_isManualRecording) {
      // Adopt the portal label only when it names something real. The
      // portal never reports which item was picked (privacy by design),
      // so a generic label ("kwin_wayland") must not become the file
      // name — the recording stays "Manual Recording".
      QString manualName;
      if (!label.isEmpty() && !isGenericSourceLabel(label)) {
        manualName = label;
      }
      if (manualName.isEmpty()) {
        manualName = QStringLiteral("Manual Recording");
      }
      if (manualName != m_currentGameName) {
        m_currentGameName = manualName;
        m_streamLabel = streamLabel();
      }
    } else {
      // Automatic recording: the capture should be the detected game
      // (silent restore / windows-only picker), but the user may still
      // have picked another window. Trust a truthful portal label over
      // the detector guess so captures of other windows aren't misnamed
      // after a merely-detected game.
      if (!label.isEmpty() && !isGenericSourceLabel(label) &&
          !sourceLabelsAgree(label, m_currentGameName)) {
        qDebug() << "RecordingManager: captured source" << label
                 << "disagrees with detected game" << m_currentGameName
                 << "- naming the recording after the captured source";
        m_currentGameName = label;
        m_streamLabel = streamLabel();
      } else {
        qDebug() << "RecordingManager: Automatic recording, keeping detected "
                    "game name:"
                 << m_currentGameName;
        // Do nothing, preserving the game name passed into startRecording
      }
    }
  }

  emit stateChanged(); // Publish the selected source label before any file can open.
  beginAudioPreparation();
}

RecordingEngine::AudioPreparation RecordingEngine::snapshotAudioPreparation() const {
  return snapshotAudio(*m_settings, m_captureSource.get(),
      m_captureGameName, m_detectedGameInfo, m_isManualRecording);
}

RecordingEngine::AudioPreparation
RecordingEngine::lookupAudioPreparation(AudioPreparation input) {
#ifdef HAVE_PIPEWIRE
  if (input.mode != "desktop") {
    input.gameNode = PipeWireAudioSource::findNodeForGame(
        input.pid, input.hints, &input.gameNodeName, &input.gameSerial);
    if (!input.voiceApp.isEmpty())
      input.voiceNode = PipeWireAudioSource::findNodeByBinary(
          input.voiceApp, &input.voiceNodeName, &input.voiceSerial);
  }
#endif
  return input;
}

void RecordingEngine::beginAudioPreparation() {
  m_audioPreparationRequested = true;
  if (!m_audioPreparationWorker.valid()) {
    m_audioPreparationRequested = false;
    m_audioPreparationGeneration = m_pipelineGeneration;
    m_audioPreparationCapture = m_captureSource.get();
    const auto input = snapshotAudioPreparation();
    m_audioPreparationWorker = std::async(std::launch::async,
        [lookup = m_audioPreparationLookup, input] { return lookup(input); });
  }
  m_operationTimer->start();
}

void RecordingEngine::pollOperations() {
  if (m_stopFinalizationWorker.valid()) {
    if (m_stopFinalizationWorker.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    if (m_audioMixer)
      for (int track = 0; track < AudioMixer::kTrackCount; ++track)
        if (m_audioMixer->sourceOperationPending(track)) return;
    m_stopFinalizationSucceeded = m_stopFinalizationWorker.get();
    finishStoppedRecording();
  }
  if (m_state == RecordingState::Stopping && m_stopNativeSettled) {
    finishStoppedRecording();
    if (m_state == RecordingState::Stopping) return;
  }
  if (!m_audioPreparationWorker.valid()) {
    m_operationTimer->stop();
    return;
  }
  if (m_audioPreparationWorker.wait_for(std::chrono::seconds(0)) !=
      std::future_status::ready)
    return;
  auto prepared = m_audioPreparationWorker.get();
  if (m_state != RecordingState::Initializing || !m_captureSource ||
      !m_captureReadyForPipeline) {
    m_audioPreparationRequested = false;
    m_operationTimer->stop();
    return;
  }
  const auto fresh = snapshotAudioPreparation();
  if (m_audioPreparationRequested || m_audioPreparationGeneration != m_pipelineGeneration ||
      m_audioPreparationCapture != m_captureSource.get() ||
      prepared.mode != fresh.mode || prepared.gameDevice != fresh.gameDevice ||
      prepared.micDevice != fresh.micDevice || prepared.voiceApp != fresh.voiceApp ||
      prepared.separate != fresh.separate || prepared.pid != fresh.pid ||
      prepared.hints != fresh.hints) {
    beginAudioPreparation();
    return;
  }
  m_preparedAudio = std::move(prepared);
  m_operationTimer->stop();
  completePipelineAndStart();
}

void RecordingEngine::completePipelineAndStart() {
  if (m_state != RecordingState::Initializing)
    return;
  if (!(m_pipelineBuilder ? m_pipelineBuilder() : buildPipeline())) {
    emit nativeError("Failed to initialize recording pipeline");
    stopRecording();
    return;
  }

  // Start timers and fully enter recording state
  m_progressTimer->start();

  setState(RecordingState::Recording);
  emit nativeStarted(m_streamLabel);

  qDebug() << "Encoded stream started:" << m_streamLabel;
}

void RecordingEngine::stopRecording() {
  // Explicit Stop dismisses continuation, but does not allow automatic
  // reacquisition of an ambiguously closed portal session.
  if (m_captureResumeRequired) {
    m_captureResumeRequired = false;
    emit captureRecoveryChanged();
  }
  if (m_portalClosurePending) {
    m_portalClosurePending = false;
    m_pendingCaptureError.clear();
  }
  const bool wasRecovering = m_captureRecoveryPending;
  cancelCaptureRecovery();
  if (wasRecovering) {
    m_pendingCaptureError.clear();
    if (m_state == RecordingState::Idle || m_state == RecordingState::Stopping)
      return;
  }
  if (m_state == RecordingState::Initializing) {
    qInfo() << "Cancelling recording initialization for" << m_currentGameName;
    m_cancelInitializing = true;
    m_captureReadyForPipeline = false;
    ++m_pipelineGeneration;
    m_audioPreparationRequested = false;
    setState(RecordingState::Stopping);
    m_progressTimer->stop();
    if (m_session) {
      const QPointer<RecordingSession> owner(m_session.get());
      connect(m_session.get(), &RecordingSession::stopFinished, this, [this, owner] {
        if (owner == m_session.get() && m_state == RecordingState::Stopping)
          finishStoppedRecording();
      }, Qt::QueuedConnection);
      m_session->requestStop();
    } else {
      if (m_captureSource) m_captureSource->prepareStopCapture();
      if (m_audioMixer) m_audioMixer->stopSourcesAsync();
      m_stopFinalizationWorker = std::async(std::launch::async,
          [capture = m_captureSource.get(), muxer = m_muxer.get()] {
        if (capture) {
          capture->finishStopCapture();
          capture->setFrameCallback(nullptr);
        }
        return !muxer || muxer->finalize();
      });
      m_operationTimer->start();
    }
    return;
  }
  if (m_state != RecordingState::Recording) {
    qWarning() << "Cannot stop recording: not recording";
    return;
  }

  setState(RecordingState::Stopping);
  m_progressTimer->stop();
  m_stopFinalizationSucceeded = true;
  if (m_session) {
    const QPointer<RecordingSession> owner(m_session.get());
    connect(m_session.get(), &RecordingSession::stopFinished, this,
            [this, owner] {
      if (m_state == RecordingState::Stopping && owner == m_session.get())
        finishStoppedRecording();
    }, Qt::QueuedConnection);
    m_session->requestStop();
  } else {
    // Retain the manager-owned muxer until this native call and owner-thread
    // completion settle. Useful also for partially constructed pipelines.
    m_stopFinalizationWorker = std::async(std::launch::async,
        [muxer = m_muxer.get()] { return !muxer || muxer->finalize(); });
    m_operationTimer->start();
  }
}

void RecordingEngine::finishStoppedRecording() {
  if (m_state != RecordingState::Stopping)
    return;
  m_stopNativeSettled = true;
  bool offerResume = false;
  if (m_portalClosurePending && !m_recordingFailed) {
    const bool sameInstance = interruptionIdentityIsCurrent();
    const auto instanceState = m_captureLifecycle.pinnedInstanceState();
    if (sameInstance && instanceState == CaptureLifecycle::InstanceState::AwaitingCensus) {
      // Resolve the launcher/census race without retrying a closed grant.
      m_operationTimer->setInterval(100);
      m_operationTimer->start();
      return;
    }
    if (sameInstance && instanceState == CaptureLifecycle::InstanceState::Running &&
        !m_shuttingDown && m_captureEndGrace.elapsed() < m_captureEndGraceMs) {
      // Session::Closed can arrive just before process exit. Capture is already
      // drained; allow the bounded exit grace before offering selection or
      // reporting a live-game interruption. Never restore the closed grant.
      m_operationTimer->setInterval(100);
      m_operationTimer->start();
      return;
    }
    offerResume = sameInstance && instanceState == CaptureLifecycle::InstanceState::Running;
    m_pendingCaptureError.clear();
    m_portalClosurePending = false;
    qInfo() << "Screen sharing session ended for" << m_captureGameName
            << (offerResume ? "- game is still tracked; select a window to resume"
                            : "- no current live game member remains");
    if (!offerResume)
      clearCaptureInterruption();
  }
  if (m_recordingFailed)
    m_portalClosurePending = false;
  if (!m_pendingCaptureError.isEmpty() && !m_recordingFailed) {
    if (capturedGameHasEnded()) {
      qInfo() << "Captured game ended; completing recording normally";
      m_pendingCaptureError.clear();
    } else if (m_captureRecoveryPending && recoveryInstanceIsCurrent()) {
      // A proven destroyed window with a surviving pinned instance is a
      // handoff, not a failed recording. Save this part before reopening.
      m_pendingCaptureError.clear();
    } else if (m_captureRecoveryPending && recoveryAwaitingCensus()) {
      // The verified launcher exited between census ticks. Native recording
      // has drained; wait for authoritative membership, not a wall-clock
      // cutoff or the dead bootstrap PID. The next census either promotes
      // a verified successor or removes the instance (runtime helpers alone
      // never keep it alive).
      m_operationTimer->setInterval(100);
      m_operationTimer->start();
      return;
    } else if (!m_shuttingDown && m_captureEndGrace.elapsed() < m_captureEndGraceMs) {
      // Native consumers have already drained. Give the detector/process exit
      // a bounded chance to confirm window shutdown before publishing Saved
      // or Error; do not keep recording dead video during that grace period.
      m_operationTimer->setInterval(100);
      m_operationTimer->start();
      return;
    } else {
      const auto error = std::exchange(m_pendingCaptureError, {});
      m_recordingFailed = true;
      qCritical() << "Pipeline error:" << error;
      emit nativeError(error);
    }
  }
  m_pendingCaptureError.clear();
  m_operationTimer->setInterval(10);
  const auto audioHistory = m_session ? m_session->audioHistory() : QJsonObject{};

  // Finalize and teardown pipeline
  const bool finalized = teardownPipeline() && m_stopFinalizationSucceeded;
  if (!finalized || m_recordingFailed || !recoveryInstanceIsCurrent())
    cancelCaptureRecovery();
  if (offerResume && finalized && !m_recordingFailed) {
    m_captureResumeRequired = true;
    emit captureRecoveryChanged();
  }
  if (m_cancelInitializing) {
    m_streamLabel.clear();
    m_currentGameName.clear();
    m_cancelInitializing = false;
    const QString authorize = std::exchange(m_pendingRestoreAuthorization, {});
    const QString authorizeIdentity = std::exchange(m_pendingRestoreIdentity, {});
    if (!authorize.isEmpty()) m_currentGameName = authorize;
    setState(RecordingState::Idle);
    if (!authorize.isEmpty()) notifyRestoreAuth(authorize, authorizeIdentity);
    emit generationFinished(finalized && !m_recordingFailed, m_captureRecoveryPending, audioHistory);
    if (m_captureRecoveryPending)
      scheduleCaptureRecovery();
    return;
  }
  const bool pipelineFailed = m_recordingFailed;
  m_recordingFailed = !finalized || pipelineFailed;

  m_streamLabel.clear();
  m_currentGameName.clear();

  setState(RecordingState::Idle);
  emit generationFinished(finalized && !pipelineFailed, m_captureRecoveryPending, audioHistory);

  if (!finalized && !pipelineFailed) {
    // Prefer the concrete reason (e.g. a stalled encoder) over the generic
    // finalize wording.
    emit nativeError(m_lastOutputError.isEmpty()
                            ? QStringLiteral("Failed to finalize recording; the output may be incomplete")
                            : m_lastOutputError);
  }

  qDebug() << "Encoded stream stopped";
  if (m_captureRecoveryPending)
    scheduleCaptureRecovery();
}

bool RecordingEngine::interruptionIdentityIsCurrent() const {
  return m_gameDetector && !m_interruptedInstance.isEmpty() &&
         m_gameDetector->captureIdentityInfo(m_interruptedGame)
                 .value("detectedGameInstance").toString() == m_interruptedInstance &&
         m_gameDetector->currentGameKey() == m_interruptedKey &&
         !m_gameDetector->isBlocked(m_interruptedKey);
}

bool RecordingEngine::requiresCaptureSelection() const {
  if (!m_gameDetector)
    return m_captureSelectionRequired;
  return m_captureLifecycle.requiresSelection();
}

void RecordingEngine::clearCaptureInterruption() {
  const bool changed = m_captureSelectionRequired || m_captureResumeRequired;
  m_captureSelectionRequired = false;
  m_captureResumeRequired = false;
  m_interruptedGame.clear();
  m_interruptedKey.clear();
  m_interruptedInstance.clear();
  if (changed)
    emit captureRecoveryChanged();
}

void RecordingEngine::setState(RecordingState newState) {
  if (m_state != newState) {
    m_state = newState;
    emit nativeStateChanged();
  }
}

bool RecordingEngine::buildPipeline() {
  qDebug() << "Building encoding pipeline...";

  if (!m_captureSource) {
    return false;
  }

  // Augment encoded file with metadata tags for later playback and indexing.
  QMap<QString, QString> outputMetadata;
  outputMetadata[QStringLiteral("title")] = m_currentGameName;
  outputMetadata[QStringLiteral("creation_time")] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  // Media kind + recorder version + local capture date/time survive
  // renames (unlike the _Replay filename suffix and timestamp), so cards
  // and the activity feed never depend on the filename.
  stampTrophyTags(outputMetadata, QStringLiteral("recording"));
  m_videoEncoder = createEncoder();
  if (!m_videoEncoder) return false;
  m_muxer = m_muxerFactory ? m_muxerFactory() : nullptr;
  if (!m_muxer) return false;
  const auto routing = configureAudio(m_audioMixer, m_preparedAudio);
  m_micTrack = routing.micTrack;
  stampTrophyEncoder(outputMetadata, m_videoEncoder->name());
  m_session = std::make_unique<RecordingSession>(m_streamLabel, m_captureSource.get(),
      m_videoEncoder.get(), m_muxer.get(), m_audioMixer,
      m_preparedAudio.separate && m_preparedAudio.mode != "desktop",
      192, routing.trackCount, outputMetadata);
  m_session->setGameAudioHints(m_preparedAudio.pid, m_preparedAudio.hints);
  m_session->setVoiceChatApp(m_preparedAudio.voiceApp);
  m_session->setVoiceTrack(routing.voiceTrack);
  m_session->setGameAudioPending(routing.gamePending);
  m_session->setDesktopAudio(m_preparedAudio.mode == "desktop");
  if (routing.gameFound) m_session->markSourceConnected(0, m_preparedAudio.gameSerial);
  if (routing.voiceFound && routing.voiceTrack >= 0)
    m_session->markSourceConnected(routing.voiceTrack, m_preparedAudio.voiceSerial);

  // Connect error signals
  const QPointer<RecordingSession> attemptSession(m_session.get());
  connect(m_session.get(), &RecordingSession::errorOccurred, this,
          [this, attemptSession](const QString &error) {
            if (m_state == RecordingState::Stopping && m_cancelInitializing)
              return;
            if (attemptSession && attemptSession == m_session.get())
              onEncodingError(error);
          }, Qt::QueuedConnection);

  // Start the session
  if (!m_session->start()) {
    qCritical() << "Failed to start recording session";
    return false;
  }

  m_activeEncoderName = m_videoEncoder ? m_videoEncoder->name() : QString();
  emit stateChanged();

  return true;
}

bool RecordingEngine::teardownPipeline() {
  ++m_pipelineGeneration;
  m_audioPreparationRequested = false;
  m_captureReadyForPipeline = false;
  qDebug() << "Tearing down recording pipeline...";

  bool sessionSucceeded = true;
  if (m_session) {
    m_session->stop();
    m_lastOutputError = m_session->outputErrorText();
    sessionSucceeded = m_session->outputSucceeded();
    m_session.reset();
  }

  // stop() flushes/finalizes through the session, but completion must also
  // inspect the muxer's retained result before destruction. finalize() is
  // idempotent, so this catches deferred/buffered output I/O failures.
  const bool finalized = !m_muxer || m_muxer->finalize();

  // Remove audio sources from mixer (they were stopped by the session)
  if (m_audioMixer) {
    for (int i = 0; i < AudioMixer::kTrackCount; ++i) {
      m_audioMixer->removeSource(i);
    }
  }

  m_muxer.reset();
  m_videoEncoder.reset();
  if (!m_activeEncoderName.isEmpty()) {
    m_activeEncoderName.clear();
    if (!m_shuttingDown) emit stateChanged();
  }
  m_captureSource.reset();
  m_micTrack = -1;
  return finalized && sessionSucceeded;
}

void RecordingEngine::applyMicrophoneDevice() {
#ifdef HAVE_PIPEWIRE
  if (!isRecording() || !m_session || !m_audioMixer || !m_settings || m_micTrack < 0) return;
  const auto device = m_settings->microphoneDevice();
  auto source = PipeWireAudioSource::createInputSource(
      device == "default" ? QString() : device, true);
  source->setStreamName(QStringLiteral("trophy-mic"));
  m_audioMixer->addSourceAsync(std::move(source), m_micTrack);
#endif
}

void RecordingEngine::applyGameAudioDevice() {
#ifdef HAVE_PIPEWIRE
  if (!isRecording() || !m_session || !m_audioMixer || !m_settings ||
      m_settings->audioMode() != "desktop") return;
  const auto device = m_settings->gameAudioDevice();
  auto source = PipeWireAudioSource::createMonitorSource(
      device == "default" ? QString() : device, true);
  source->setStreamName(QStringLiteral("trophy-desktop"));
  m_audioMixer->addSourceAsync(std::move(source), 0);
#endif
}

void RecordingEngine::applyVoiceChatApp() {
  if (!isRecording() || !m_session || !m_audioMixer || !m_settings ||
      m_settings->audioMode() == "desktop") return;
  auto track = m_session->audioRoutingState().voiceTrack;
  const auto app = m_settings->voiceChatApp().trimmed();
  m_session->setVoiceChatApp(app);
  if (!app.isEmpty() && track < 0 && m_settings->separateAudioTracks()) {
    track = 1;
    m_session->setVoiceTrack(track);
  }
  if (track >= 0) {
    m_audioMixer->removeSourceAsync(track);
    m_session->forgetTrack(track);
  }
}

QString RecordingEngine::streamLabel() const {
  return QStringLiteral("encoded-stream-%1").arg(m_pipelineGeneration);
}

QString RecordingEngine::activeEncoderName() const {
  return m_activeEncoderName;
}

void RecordingEngine::updateProgress() {
  // Read the atomic latch on the owner thread, never tear down from a
  // muxer/audio/video callback. stop() still drains before inspecting it.
  if (m_state == RecordingState::Recording && m_session &&
      m_session->outputFailed()) {
    const QString reason = m_session->outputErrorText();
    onEncodingError(reason.isEmpty()
                        ? QStringLiteral("Failed to write recording; the output may be incomplete")
                        : reason);
    return;
  }

  emit nativeProgress();
}

bool RecordingEngine::capturedGameHasEnded() const {
  if (!m_gameDetector || m_isManualRecording || m_captureGameName != m_currentGameName)
    return false;
  const auto instance =
      m_detectedGameInfo.value("detectedGameInstance").toString();
  return !instance.isEmpty() && !m_gameDetector->instanceAlive(instance) &&
         !(m_captureRecoveryPending && recoveryAwaitingCensus());
}

bool RecordingEngine::recoveryInstanceIsCurrent() const {
  return !m_shuttingDown && m_captureLifecycle.instanceIsCurrent();
}

bool RecordingEngine::recoveryAwaitingCensus() const {
  return !m_shuttingDown && m_captureLifecycle.awaitingCensus();
}

void RecordingEngine::cancelCaptureRecovery(const QString &reason) {
  m_captureLifecycle.cancelRetry();
  if (m_captureRecoveryPending) {
    m_captureRecoveryPending = false;
    qInfo() << "Capture recovery ended for" << m_captureLifecycle.game()
            << "-" << reason;
    emit captureRecoveryChanged();
  }
}

void RecordingEngine::scheduleCaptureRecovery() {
  qInfo() << "Captured window closed; scheduling capture recovery for" << m_captureLifecycle.game();
  m_captureLifecycle.scheduleRetry(m_recoveryDelayMs,
      [this] { return !m_shuttingDown && m_state == RecordingState::Idle; },
      [this] {
    if (!m_captureRecoveryPending || m_shuttingDown) return;
    const QString game = m_captureLifecycle.game();
    startRecording(game);
  }, [this] {
    cancelCaptureRecovery(QStringLiteral("instance ended or no longer eligible"));
  });
}

void RecordingEngine::onCaptureError(const QString &error) {
  // A queued source-loss event may trail an explicit stop/gameClosed. Output
  // failures still use onEncodingError and cannot be hidden by this guard.
  if (m_state == RecordingState::Stopping || m_state == RecordingState::Idle)
    return;
  if (m_captureSource)
    qDebug() << "Capture error classification:"
             << static_cast<int>(m_captureSource->lastErrorKind()) << error;
  if (m_state == RecordingState::Initializing && m_explicitCaptureSelectionAttempt &&
      m_captureSource &&
      m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::Cancelled) {
    // The user cancelled a fresh picker. Keep the automatic-selection fence,
    // but do not turn that explicit cancellation into a pipeline failure.
    stopRecording();
    return;
  }
  if (m_captureSource &&
      m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::SessionClosed) {
    m_interruptedGame = m_currentGameName;
    m_interruptedKey = m_detectedGameInfo.value("detectedGameKey").toString();
    m_interruptedInstance = m_detectedGameInfo.value("detectedGameInstance").toString();
    m_pendingCaptureError = error;
    m_captureEndGrace.start();
    stopRecording();
    m_portalClosurePending = true;
    m_captureSelectionRequired = !m_isManualRecording && interruptionIdentityIsCurrent();
    if (m_captureSelectionRequired)
      m_captureLifecycle.fenceSelection(m_interruptedInstance);
    emit captureRecoveryChanged();
    return;
  }
  if (m_captureSource &&
      (m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::SourceDestroyed ||
       (m_state == RecordingState::Initializing &&
         m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::SourceNotReady)) &&
      !requiresCaptureSelection() &&
      !m_isManualRecording && m_gameDetector && m_captureGameName == m_currentGameName) {
    m_captureLifecycle.pin(m_currentGameName, m_detectedGameInfo);
    if (m_captureLifecycle.canRecover(m_captureSource->lastErrorKind(),
                                     m_state == RecordingState::Initializing)) {
      if (m_state == RecordingState::Recording) {
        m_pendingCaptureError = error;
        m_captureEndGrace.start();
      }
      // An approved empty retry still belongs to the same continuation.
      // Retain feedback for the last saved part until recovery truly ends.
      stopRecording();
      m_captureRecoveryPending = true;
      emit captureRecoveryChanged();
      return;
    }
  }
  if (m_state == RecordingState::Recording && m_captureSource &&
      (m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::SourceUnavailable ||
       m_captureSource->lastErrorKind() == IVideoCaptureSource::CaptureErrorKind::SourceDestroyed) &&
      !m_isManualRecording && m_gameDetector && m_captureGameName == m_currentGameName &&
      !m_detectedGameInfo.value("detectedGameKey").toString().isEmpty() &&
      m_detectedGameInfo.value("detectedGamePid").toLongLong() > 0) {
    m_pendingCaptureError = error;
    m_captureEndGrace.start();
    stopRecording();
    return;
  }
  onEncodingError(error);
}

void RecordingEngine::onEncodingError(const QString &error) {
  qCritical() << "Pipeline error:" << error;
  if (m_state == RecordingState::Recording || m_state == RecordingState::Stopping)
    m_recordingFailed = true;
  emit nativeError(error);

  if (m_state == RecordingState::Initializing) {
    stopRecording();
  } else if (m_state == RecordingState::Recording) {
    stopRecording();
  }
}

} // namespace TrophyBackend
