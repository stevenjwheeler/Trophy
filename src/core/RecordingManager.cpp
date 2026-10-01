/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingManager.h"
#include "RecordingSession.h"
#include "CallbackSink.h"
#include "CircularFrameBuffer.h"
#include "StorageManager.h"
#include "WindowService.h"
#include "audio/AudioMixer.h"
#include <QMap>
#include <qlogging.h>
#ifdef HAVE_PIPEWIRE
#include "audio/PipeWireAudioSource.h"
#endif

extern "C" {
#include <libavutil/pixfmt.h>
}
#include "capture/CaptureFactory.h"
#include "capture/IVideoCaptureSource.h"
#include "detection/GameDetectionService.h"
#include "encoding/EncoderFactory.h"
#include "encoding/IEncoder.h"
#include "muxing/FFmpegMuxer.h"
#include "settings/SettingsManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <atomic>

namespace TrophyBackend {

RecordingManager::RecordingManager(QObject *parent) : QObject(parent) {
  // Progress update timer
  m_progressTimer = new QTimer(this);
  m_progressTimer->setInterval(1000); // Update every second
  connect(m_progressTimer, &QTimer::timeout, this,
          &RecordingManager::updateProgress);
}

RecordingManager::~RecordingManager() {
  if (m_state == RecordingState::Recording) {
    stopRecording();
  }
}

void RecordingManager::setSettingsManager(SettingsManager *settings) {
  m_settings = settings;
}

void RecordingManager::setStorageManager(StorageManager *storage) {
  m_storage = storage;
}

void RecordingManager::setAudioMixer(AudioMixer *mixer) {
  m_audioMixer = mixer;
}

void RecordingManager::setWindowService(WindowService *ws) {
  m_windowService = ws;
}

void RecordingManager::setGameDetectionService(GameDetectionService *detector) {
  m_gameDetector = detector;
}

void RecordingManager::startRecording(const QString &gameName,
                                      unsigned long windowId /*= 0*/) {
  if (m_state != RecordingState::Idle) {
    qWarning() << "Cannot start recording: not in Idle state";
    return;
  }

  qDebug() << "startRecording called. gameName:" << gameName
           << "windowId:" << windowId;

  m_manualWindowId = windowId;

  if (!m_settings || !m_storage) {
    emit recordingError("Recording manager not properly initialized");
    return;
  }

  m_isManualRecording = gameName.isEmpty();

  if (m_isManualRecording && m_manualWindowId != 0 && m_windowService) {
    m_currentGameName = m_windowService->getWindowTitle(m_manualWindowId);
    if (m_currentGameName.isEmpty()) {
      m_currentGameName = "Recording";
    }
  } else {
    m_currentGameName = gameName.isEmpty() ? "Recording" : gameName;
  }
  emit currentGameNameChanged();

  setState(RecordingState::Initializing);

  // Check storage space and prevent starting if the limit cannot be satisfied.
  if (!m_storage->enforceStorageLimit()) {
    qWarning() << "Storage limit cannot be satisfied, aborting recording start";
    setState(RecordingState::Idle);
    return;
  }

  // Generate output path
  m_currentFilePath = generateOutputPath();
  if (m_storage) {
    m_storage->setExcludedRecordingPath(m_currentFilePath);
  }

  // Ensure output directory exists
  QDir dir = QFileInfo(m_currentFilePath).dir();
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      emit recordingError("Failed to create output directory");
      setState(RecordingState::Idle);
      return;
    }
  }

  // 1. Create and initialize capture source
  m_captureSource = CaptureFactory::createBestSource();

  // Distinguish manual vs automatic for restore token usage
  bool isManual = gameName.isEmpty();

  if (!m_captureSource) {
    emit recordingError("Failed to create capture source");
    setState(RecordingState::Idle);
    return;
  }

  // Connect ready signal to finish the pipeline
  connect(m_captureSource.get(), &IVideoCaptureSource::ready, this,
          &RecordingManager::finishPipelineAndStart, Qt::QueuedConnection);
  connect(m_captureSource.get(), &IVideoCaptureSource::captureError, this,
          &RecordingManager::onEncodingError, Qt::QueuedConnection);

  // Connect restore token update to save it for this game.
  // Tokens are held indefinitely and only overwritten with fresher ones —
  // never pruned for missing recordings.
  connect(m_captureSource.get(), &IVideoCaptureSource::restoreTokenUpdated,
          this, [this](const QString &token) {
            if (m_settings && !m_currentGameName.isEmpty() &&
                m_currentGameName != "Recording") {
              qDebug() << "Saving Wayland restore token for game:"
                       << m_currentGameName
                       << "(fp:" << token.left(4) << ")";
              m_settings->setRestoreToken(m_currentGameName, token);
            }
          });
  connect(m_captureSource.get(), &IVideoCaptureSource::restoreTokenStale, this,
          [this](const QString &oldToken) {
            Q_UNUSED(oldToken)
            qWarning() << "Saved restore token rejected for"
                       << m_currentGameName << "- re-authorizing";
            teardownPipeline();
            emit restoreAuthNeeded(m_currentGameName);
          });

  CaptureConfig capConfig;
  capConfig.targetFps = m_settings->fps();
  capConfig.windowId = m_manualWindowId;
  capConfig.useDmaBuf = m_settings->useDmaBuf();
  // Game-targeted (non-manual) recordings get a windows-only picker;
  // manual recordings keep the full monitor + window choice.
  capConfig.windowOnly = !isManual;

  // Use restore token only for automatic recordings, always prompt for manual
  if (!isManual) {
    capConfig.waylandRestoreToken =
        m_settings->getRestoreToken(m_currentGameName);
    if (!capConfig.waylandRestoreToken.isEmpty()) {
      qDebug() << "Using saved Wayland restore token for:" << m_currentGameName
               << "(fp:" << capConfig.waylandRestoreToken.left(4) << ")";
    }
  }

  if (!m_captureSource->initialize(capConfig)) {
    emit recordingError("Failed to initialize capture source");
    teardownPipeline();
    setState(RecordingState::Idle);
    return;
  }

  qDebug() << "Capture source initialized, starting capture...";
  if (!m_captureSource->startCapture()) {
    emit recordingError("Failed to start capture source");
    teardownPipeline();
    setState(RecordingState::Idle);
    return;
  }

  // We wait for the 'ready' signal to initialize the rest of the pipeline
  qDebug() << "Waiting for capture source to be ready...";
}

void RecordingManager::finishPipelineAndStart() {
  if (m_state != RecordingState::Initializing) {
    return;
  }

  qDebug() << "Capture source ready, finishing pipeline...";

  if (m_captureSource) {
    QString label = m_captureSource->sourceLabel();

    // Wayland Fallback: If XDG Portal/PipeWire hides the application name,
    // query the GameDetectionService for the foreground active game.
    if (label.isEmpty() || label.toLower().contains("kwin") ||
        label.toLower().contains("wayland") || label == "Unknown Game") {
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
      if (!label.isEmpty()) {
        m_currentGameName = label;
        emit currentGameNameChanged();
        m_currentFilePath = generateOutputPath();
      } else if (m_manualWindowId != 0 && m_windowService) {
        // Fallback for manual capture where capture source provides empty
        // label
        m_currentGameName = m_windowService->getWindowTitle(m_manualWindowId);
        if (!m_currentGameName.isEmpty()) {
          emit currentGameNameChanged();
          m_currentFilePath = generateOutputPath();
        }
      }
    } else {
      qDebug() << "RecordingManager: Automatic recording, keeping detected game "
                 "name:"
              << m_currentGameName;
      // Do nothing, preserving the game name passed into startRecording
    }
  }

  if (!buildPipeline()) {
    emit recordingError("Failed to initialize recording pipeline");
    teardownPipeline();
    setState(RecordingState::Idle);
    return;
  }

  // Start timers and fully enter recording state
  m_recordingTimer.start();
  m_progressTimer->start();

  setState(RecordingState::Recording);
  emit recordingStarted(m_currentFilePath);

  qInfo() << "Recording started:" << m_currentFilePath;
}

void RecordingManager::stopRecording() {
  if (m_state != RecordingState::Recording) {
    qWarning() << "Cannot stop recording: not recording";
    return;
  }

  setState(RecordingState::Stopping);
  m_progressTimer->stop();

  // Finalize and teardown pipeline
  teardownPipeline();

  if (m_storage) {
    m_storage->clearExcludedRecordingPath();
  }

  QString filePath = m_currentFilePath;
  m_currentFilePath.clear();
  m_currentGameName.clear();
  m_manualWindowId = 0; // Reset manual window ID

  setState(RecordingState::Idle);
  if (m_storage) {
    m_storage->refresh();
  }

  // Duration + size for the "Recording Saved" notice (file is finalized by
  // now, so read the size off disk).
  const qint64 durationMs =
      m_recordingTimer.isValid() ? m_recordingTimer.elapsed() : 0;
  const qint64 sizeBytes = QFileInfo(filePath).size();
  emit recordingStopped(filePath, durationMs, sizeBytes);
  emit currentGameNameChanged();

  qInfo() << "Recording stopped:" << filePath;
}

void RecordingManager::setState(RecordingState newState) {
  if (m_state != newState) {
    m_state = newState;
    emit stateChanged(m_state);
  }
}

bool RecordingManager::buildPipeline() {
  qDebug() << "Building encoding pipeline...";

  if (!m_captureSource) {
    return false;
  }

  // Create encoder based on settings and available hardware
  QString requestedEncoder = m_settings->encoder();
  m_videoEncoder = EncoderFactory::create(requestedEncoder);
  if (!m_videoEncoder) {
    qCritical() << "Failed to create video encoder";
    return false;
  }

  // Create muxer
  m_muxer = std::make_unique<FFmpegMuxer>();

  // Muxer doesn't need much early configuration, but capture and encoder do

  EncoderConfig encConfig;

  // Logic to determine target resolution
  // Assuming your settings->resolution() returns something like "720p",
  // "1080p", or "Native"
  QString targetRes =
      m_settings->videoResolution().toLower(); // Make case-insensitive

  if (targetRes.contains('x')) {
    // Handles "1280x720", "1920x1080", etc.
    QStringList parts = targetRes.split('x');
    if (parts.size() == 2) {
      encConfig.width = parts[0].toInt();
      encConfig.height = parts[1].toInt();
    }
  } else {
    // Native mode
    QSize nativeRes = m_captureSource->nativeResolution();
    encConfig.width = nativeRes.width();
    encConfig.height = nativeRes.height();

    // Update auto resolution in settings for UI display
    if (m_settings) {
      m_settings->setAutoResolutionWidth(encConfig.width);
      m_settings->setAutoResolutionHeight(encConfig.height);
    }

    // CRITICAL: Ensure dimensions are even. FFmpeg/YUV420p hates odd numbers.
    if (encConfig.width % 2 != 0)
      encConfig.width--;
    if (encConfig.height % 2 != 0)
      encConfig.height--;
  }

  encConfig.fps = m_settings->fps();
  encConfig.bitrate = m_settings->videoBitrate();

  encConfig.maxBitrate = m_settings->maxVideoBitrate();
  encConfig.useVBR = m_settings->useVBR();

  encConfig.preset = m_settings->encoderPreset();
  encConfig.inputPixelFormat = AV_PIX_FMT_BGRA;

  if (!m_videoEncoder->initialize(encConfig)) {
    qCritical() << "Failed to initialize video encoder";
    return false;
  }

  // Set up audio sources in the mixer before starting the session
#ifdef HAVE_PIPEWIRE
  int activeAudioTrackCount = 0;
  QString gameProcess;
  QString vcApp;
  bool gameSourceAdded = false;
  bool vcSourceAdded = false;

  if (m_audioMixer) {
    QString mode = m_settings->audioMode();

    if (mode == "desktop") {
      // Track 0: all system audio (sink monitor)
      QString gameDeviceId = m_settings->gameAudioDevice();
      qDebug() << "Desktop mode: using game audio device:" << gameDeviceId;
      m_audioMixer->addSource(
          PipeWireAudioSource::createMonitorSource(
              gameDeviceId == "default" ? QString() : gameDeviceId),
          0);
      activeAudioTrackCount++;
      // Track 1: microphone
      QString micDeviceId = m_settings->microphoneDevice();
      qDebug() << "Desktop mode: using microphone device:" << micDeviceId;
      m_audioMixer->addSource(
          PipeWireAudioSource::createInputSource(
              micDeviceId == "default" ? QString() : micDeviceId),
          1);
      activeAudioTrackCount++;

    } else { // "game" mode
      // Separate mode uses fixed positions so the MKV always has 3 named audio
      // streams (game=0, voice=1, mic=2) regardless of what's running.
      // Merged mode uses contiguous positions so the accumulator has no gaps.
      const bool isSeparate = m_settings->separateAudioTracks();
      int nextContiguous = 0;
      int activeSources = 0;

      // === Game audio (always at track 0) ===
      uint32_t targetPid = 0;
      if (m_manualWindowId != 0 && m_windowService) {
        gameProcess =
            m_windowService->getProcessNameForWindow(m_manualWindowId);
      } else if (m_captureSource) {
        targetPid = m_captureSource->sourcePid();
      }

      {
        const int gameTrack = isSeparate ? 0 : nextContiguous++;
        uint32_t nodeId = 0xFFFFFFFFu;

        QString nodeName, nodeSerial;

        // Try PID search first if we have one (more accurate on Wayland)
        if (targetPid != 0) {
          nodeId = PipeWireAudioSource::findNodeByPid(targetPid, &nodeName,
                                                      &nodeSerial);
          if (nodeId != 0xFFFFFFFFu)
            qDebug() << "Game audio: found node via PID" << targetPid;
        }

        // Try process name / app id from capture source
        if (nodeId == 0xFFFFFFFFu && m_captureSource) {
          QString hint = m_captureSource->sourceProcessName();
          if (!hint.isEmpty()) {
            nodeId = PipeWireAudioSource::findNodeByProcessName(hint, &nodeName,
                                                                &nodeSerial);
            if (nodeId != 0xFFFFFFFFu)
              qDebug() << "Game audio: found node via capture source hint"
                      << hint;
          }
        }

        // Try derived process name (standard behavior)
        if (nodeId == 0xFFFFFFFFu && !gameProcess.isEmpty()) {
          nodeId = PipeWireAudioSource::findNodeByProcessName(
              gameProcess, &nodeName, &nodeSerial);
          if (nodeId != 0xFFFFFFFFu)
            qDebug() << "Game audio: found node via process name" << gameProcess;
        }

        if (nodeId != 0xFFFFFFFFu) {
          m_audioMixer->addSource(PipeWireAudioSource::createApplicationSource(
                                      nodeId, nodeName, nodeSerial),
                                  gameTrack);
          activeSources++;
          gameSourceAdded = true;
        } else {
          // Fallback to default monitor (whole system audio) if no specific
          // game found
          qDebug() << "Game audio: no specific node found, falling back to "
                        "default monitor for track"
                     << gameTrack;
          m_audioMixer->addSource(
              PipeWireAudioSource::createMonitorSource(QString()), gameTrack);
          gameSourceAdded = true;
          activeSources++;
        }
      }

      // === Voice chat (track 1 in separate; next contiguous slot in merged)
      // ===
      vcApp = m_settings->voiceChatApp().trimmed();
      if (!vcApp.isEmpty()) {
        QString vcNodeName, vcSerial;
        uint32_t nodeId = PipeWireAudioSource::findNodeByProcessName(
            vcApp, &vcNodeName, &vcSerial);
        if (nodeId != 0xFFFFFFFF) {
          const int voiceTrack = isSeparate ? 1 : nextContiguous++;
          m_audioMixer->addSource(PipeWireAudioSource::createApplicationSource(
                                      nodeId, vcNodeName, vcSerial),
                                  voiceTrack);
          activeSources++;
          vcSourceAdded = true;
        } else {
          qDebug() << "No PipeWire node for voice chat app:" << vcApp;
        }
      }

      // === Microphone (track 2 in separate; next contiguous slot in merged)
      // ===
      {
        const int micTrack = isSeparate ? 2 : nextContiguous++;
        m_audioMixer->addSource(PipeWireAudioSource::createInputSource(
                                    m_settings->microphoneDevice() == "default"
                                        ? QString()
                                        : m_settings->microphoneDevice()),
                                micTrack);
        activeSources++;
      }

      // Separate: always 3 streams so the file layout is consistent.
      // Merged: only wait for the sources that are actually running.
      activeAudioTrackCount = isSeparate ? 3 : activeSources;
    }
  }
#else
  int activeAudioTrackCount = 0;
#endif

  // Augment encoded file with metadata tags for later playback and indexing.
  QMap<QString, QString> outputMetadata;
  outputMetadata[QStringLiteral("title")] = m_currentGameName;
  outputMetadata[QStringLiteral("creation_time")] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

  // Create recording session to coordinate everything
    m_session = std::make_unique<RecordingSession>(
      m_currentFilePath, m_captureSource.get(), m_videoEncoder.get(),
      m_muxer.get(), m_audioMixer, m_settings->separateAudioTracks(),
      192, // Audio bitrate: hard-coded to 192 kbps (CD quality)
      activeAudioTrackCount, outputMetadata);

  // Create a FrameDispatcher and provide a CircularFrameBuffer sink for clips
  auto dispatcher = std::make_shared<TrophyBackend::FrameDispatcher>();
  auto bufferSeconds = std::max(1, m_settings->clipBufferSeconds());
  auto circular = std::make_shared<TrophyBackend::CircularFrameBuffer>(static_cast<size_t>(m_settings->fps() * bufferSeconds));
  circular->start();
  dispatcher->registerSink(circular);
  // Register encoder sink that forwards frames to the recording session queue
  auto encoderSink = std::make_shared<TrophyBackend::CallbackSink>(
      [this](TrophyBackend::FrameDescriptor &&frame) -> bool {
          // Convert FrameDescriptor to VideoFrame and then to QueuedFrame
          TrophyBackend::VideoFrame vf{};
          vf.data = nullptr;
          vf.width = static_cast<int>(frame.width);
          vf.height = static_cast<int>(frame.height);
          vf.stride = 0;
          vf.timestamp = static_cast<int64_t>(frame.pts);
          // Prefer explicit encoded pixel format if provided; otherwise use
          // the DRM/format field as a best-effort fallback.
          vf.pixelFormat = frame.pixelFormat != 0 ? static_cast<int>(frame.pixelFormat) : static_cast<int>(frame.format);
          vf.isDmaBuf = true;
          vf.dmaBufFd = frame.fd;
          vf.drmFormat = frame.format;
          vf.drmModifier = frame.drmModifier;
          vf.dmaBufStride = frame.dmaBufStride;
          vf.dmaBufOffset = frame.dmaBufOffset;
          TrophyBackend::QueuedFrame qf(vf);
          bool pushed = m_session->pushQueuedFrame(std::move(qf));
          // Per-frame success logs flood at 60fps; always log drops, and
          // throttle successes unless TROPHY_VIDEO_DEBUG is set.
          static std::atomic<uint64_t> s_sinkFrames{0};
          const uint64_t n = ++s_sinkFrames;
          if (!pushed || qEnvironmentVariableIsSet("TROPHY_VIDEO_DEBUG") ||
              n == 1 || n % 600 == 0) {
            qDebug() << "Encoder sink: pushed frame to session?" << pushed << "pts:" << vf.timestamp << "fd:" << vf.dmaBufFd << "pixfmt:" << vf.pixelFormat;
          }
          return pushed;
      });
  dispatcher->registerSink(encoderSink);
  // Provide dispatcher to session
  m_session->setFrameDispatcher(dispatcher);

  // Set app names for periodic node checking during recording
  m_session->setGameApps(gameProcess);
  m_session->setVoiceChatApp(vcApp);

#ifdef HAVE_PIPEWIRE
  // If sources were already found and added during buildPipeline, mark them as
  // connected so the node-check timer doesn't replace the already-running
  // sources.
  if (gameSourceAdded)
    m_session->markSourceConnected(0);
  if (vcSourceAdded)
    m_session->markSourceConnected(1);
#endif

  // Connect error signals
  connect(m_session.get(), &RecordingSession::errorOccurred, this,
          &RecordingManager::onEncodingError);

  // Start the session
  if (!m_session->start()) {
    qCritical() << "Failed to start recording session";
    return false;
  }

  emit activeEncoderNameChanged();

  return true;
}

void RecordingManager::teardownPipeline() {
  qDebug() << "Tearing down recording pipeline...";

  if (m_session) {
    m_session->stop();
    m_session.reset();
  }

  // Remove audio sources from mixer (they were stopped by the session)
  if (m_audioMixer) {
    for (int i = 0; i < AudioMixer::kTrackCount; ++i) {
      m_audioMixer->removeSource(i);
    }
  }

  m_muxer.reset();
  m_videoEncoder.reset();
  m_captureSource.reset();
}

QString RecordingManager::generateOutputPath() const {
  QString basePath = m_settings->recordingsPath();
  QString format = m_settings->containerFormat();
  QString timestamp =
      QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");

  QString filename;
  if (!m_currentGameName.isEmpty()) {
    // Sanitize game name for filename
    QString safeName = m_currentGameName;
    safeName.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    // Collapse consecutive underscores/dashes
    safeName.replace(QRegularExpression("[_-]{2,}"), "_");
    // Remove leading/trailing garbage
    safeName =
        safeName.trimmed().replace(QRegularExpression("^[_-]+|[_-]+$"), "");

    if (safeName.isEmpty() || safeName == "Recording")
      safeName = m_currentGameName == "Recording" ? "Recording" : "Game";

    filename = QString("%1_%2.%3").arg(safeName, timestamp, format);
  } else {
    filename = QString("Recording_%1.%2").arg(timestamp, format);
  }

  return basePath + "/" + filename;
}

qint64 RecordingManager::recordingDuration() const {
  if (m_state == RecordingState::Recording) {
    return m_recordingTimer.elapsed();
  }
  return 0;
}

qint64 RecordingManager::recordingSize() const {
  // While recording, prefer the muxer's byte counter (AVIO position,
  // updated per packet) over the on-disk size: encoder queues,
  // interleave buffering and AVIO buffering all delay bytes reaching
  // disk, so stat() can lag the true size by megabytes.
  if (m_session && m_state == RecordingState::Recording) {
    const qint64 live = m_session->bytesWritten();
    if (live > 0) {
      return live;
    }
  }
  if (m_currentFilePath.isEmpty()) {
    return 0;
  }
  QFileInfo info(m_currentFilePath);
  return info.exists() ? info.size() : 0;
}

QString RecordingManager::activeEncoderName() const {
  if (m_videoEncoder) {
    return m_videoEncoder->name();
  }
  return "";
}

QString RecordingManager::detectedEncoderName() const {
  auto encoders = EncoderFactory::detectAvailableEncoders();
  if (!encoders.empty()) {
    return encoders[0].description;
  }
  return "Software";
}

void RecordingManager::updateProgress() {
  // Poll for window title changes during manual-window recordings
  if (m_manualWindowId != 0 && m_windowService) {
    QString newTitle = m_windowService->getWindowTitle(m_manualWindowId);
    if (!newTitle.isEmpty() && newTitle != m_currentGameName) {
      m_currentGameName = newTitle;
      emit currentGameNameChanged();
    }
  }

  emit progressUpdated(recordingDuration(), recordingSize());
}

void RecordingManager::onEncodingError(const QString &error) {
  qCritical() << "Pipeline error:" << error;
  emit recordingError(error);

  if (m_state == RecordingState::Initializing) {
    teardownPipeline();
    setState(RecordingState::Idle);
  } else {
    stopRecording();
  }
}

} // namespace TrophyBackend
