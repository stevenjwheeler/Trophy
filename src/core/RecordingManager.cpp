/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingManager.h"
#include "RecordingSession.h"
#include "StorageManager.h"
#include "WindowService.h"
#include "audio/AudioMixer.h"
#ifdef HAVE_PIPEWIRE
#include "audio/PipeWireAudioSource.h"
#endif

extern "C" {
#include <libavutil/pixfmt.h>
}
#include "capture/CaptureFactory.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/EncoderFactory.h"
#include "encoding/IEncoder.h"
#include "muxing/FFmpegMuxer.h"
#include "settings/SettingsManager.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

namespace GameRecorder {

RecordingManager::RecordingManager(QObject *parent) : QObject(parent) {
  // Progress update timer
  m_progressTimer = new QTimer(this);
  m_progressTimer->setInterval(1000); // Update every second
  connect(m_progressTimer, &QTimer::timeout, this,
          &RecordingManager::updateProgress);
}

RecordingManager::~RecordingManager() {
  if (m_state == RecordingState::Recording ||
      m_state == RecordingState::Paused) {
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

void RecordingManager::startRecording(const QString &gameName,
                                      unsigned long windowId /*= 0*/) {
  if (m_state != RecordingState::Idle) {
    qWarning() << "Cannot start recording: not in Idle state";
    return;
  }

  m_manualWindowId = windowId;

  if (!m_settings || !m_storage) {
    emit recordingError("Recording manager not properly initialized");
    return;
  }

  m_currentGameName = gameName.isEmpty() ? "Recording" : gameName;
  emit currentGameNameChanged();

  setState(RecordingState::Initializing);

  // Check storage space
  m_storage->enforceStorageLimit();

  // Generate output path
  m_currentFilePath = generateOutputPath();

  // Ensure output directory exists
  QDir dir = QFileInfo(m_currentFilePath).dir();
  if (!dir.exists()) {
    if (!dir.mkpath(".")) {
      emit recordingError("Failed to create output directory");
      setState(RecordingState::Idle);
      return;
    }
  }

  // Build the recording pipeline
  if (!buildPipeline()) {
    emit recordingError("Failed to initialize recording pipeline");
    teardownPipeline();
    setState(RecordingState::Idle);
    return;
  }

  // Start recording
  m_recordingTimer.start();
  m_progressTimer->start();

  setState(RecordingState::Recording);
  emit recordingStarted(m_currentFilePath);

  qInfo() << "Recording started:" << m_currentFilePath;
}

void RecordingManager::stopRecording() {
  if (m_state != RecordingState::Recording &&
      m_state != RecordingState::Paused) {
    qWarning() << "Cannot stop recording: not recording";
    return;
  }

  setState(RecordingState::Stopping);
  m_progressTimer->stop();

  // Finalize and teardown pipeline
  teardownPipeline();

  QString filePath = m_currentFilePath;
  m_currentFilePath.clear();
  m_currentGameName.clear();
  m_manualWindowId = 0; // Reset manual window ID

  setState(RecordingState::Idle);
  if (m_storage) {
    m_storage->refresh();
  }

  emit recordingStopped(filePath);
  emit currentGameNameChanged();

  qInfo() << "Recording stopped:" << filePath;
}

void RecordingManager::pauseRecording() {
  if (m_state != RecordingState::Recording) {
    qWarning() << "Cannot pause: not recording";
    return;
  }

  // TODO: Implement pause logic in capture/encoding
  setState(RecordingState::Paused);
  qInfo() << "Recording paused";
}

void RecordingManager::resumeRecording() {
  if (m_state != RecordingState::Paused) {
    qWarning() << "Cannot resume: not paused";
    return;
  }

  // TODO: Implement resume logic in capture/encoding
  setState(RecordingState::Recording);
  qInfo() << "Recording resumed";
}

void RecordingManager::setState(RecordingState newState) {
  if (m_state != newState) {
    m_state = newState;
    emit stateChanged(m_state);
  }
}

bool RecordingManager::buildPipeline() {
  qInfo() << "Building recording pipeline...";

  // Create capture source based on display server
  if (m_manualWindowId != 0) {
    qInfo() << "Manual window ID provided, forcing X11 capture source";
    m_captureSource = CaptureFactory::createX11Source();
  } else {
    m_captureSource = CaptureFactory::createBestSource();
  }

  if (!m_captureSource) {
    qCritical() << "Failed to create capture source";
    return false;
  }

  // Create encoder based on settings and available hardware
  QString requestedEncoder = m_settings->encoder();
  m_videoEncoder = EncoderFactory::create(requestedEncoder);
  if (!m_videoEncoder) {
    qCritical() << "Failed to create video encoder";
    return false;
  }

  qInfo() << "Using encoder:" << m_videoEncoder->name();

  // Create muxer
  m_muxer = std::make_unique<FFmpegMuxer>();

  // Configure components
  CaptureConfig capConfig;
  capConfig.targetFps = m_settings->fps();
  // Use manual window ID if provided, otherwise default to 0 (root window)
  capConfig.windowId = m_manualWindowId;

  if (!m_captureSource->initialize(capConfig)) {
    qCritical() << "Failed to initialize capture source";
    return false;
  }

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
  QString windowClass;
  QString vcApp;
  bool gameSourceAdded = false;
  bool vcSourceAdded = false;

  if (m_audioMixer) {
    QString mode = m_settings->audioMode();

    if (mode == "desktop") {
      // Track 0: all system audio (sink monitor)
      QString gameDeviceId = m_settings->gameAudioDevice();
      qInfo() << "Desktop mode: using game audio device:" << gameDeviceId;
      m_audioMixer->addSource(PipeWireAudioSource::createMonitorSource(
          gameDeviceId == "default" ? QString() : gameDeviceId), 0);
      activeAudioTrackCount++;
      // Track 1: microphone
      QString micDeviceId = m_settings->microphoneDevice();
      qInfo() << "Desktop mode: using microphone device:" << micDeviceId;
      m_audioMixer->addSource(PipeWireAudioSource::createInputSource(
          micDeviceId == "default" ? QString() : micDeviceId), 1);
      activeAudioTrackCount++;

    } else { // "game" mode
      // Separate mode uses fixed positions so the MKV always has 3 named audio
      // streams (game=0, voice=1, mic=2) regardless of what's running.
      // Merged mode uses contiguous positions so the accumulator has no gaps.
      const bool isSeparate = m_settings->separateAudioTracks();
      int nextContiguous = 0;
      int activeSources = 0;

      // === Game audio (always at track 0) ===
      if (m_manualWindowId != 0 && m_windowService) {
          gameProcess = m_windowService->getProcessNameForWindow(m_manualWindowId);
          windowClass = m_windowService->getWindowClassForWindow(m_manualWindowId);
      }

      {
        const int gameTrack = isSeparate ? 0 : nextContiguous++;
        uint32_t nodeId = 0xFFFFFFFFu;

        QString nodeName, nodeSerial;

        // Try PID-based process name first
        if (!gameProcess.isEmpty()) {
          nodeId = PipeWireAudioSource::findNodeByProcessName(gameProcess, &nodeName, &nodeSerial);
          if (nodeId != 0xFFFFFFFFu)
            qInfo() << "Game audio: found node via process name" << gameProcess;
        }

        // Fall back to X11 window class name (handles Spotify and similar apps
        // whose _NET_WM_PID points to a kernel worker thread)
        if (nodeId == 0xFFFFFFFFu && !windowClass.isEmpty()) {
          qInfo() << "Trying window class name for PipeWire lookup:" << windowClass;
          nodeId = PipeWireAudioSource::findNodeByProcessName(windowClass, &nodeName, &nodeSerial);
          if (nodeId != 0xFFFFFFFFu)
            qInfo() << "Game audio: found node via window class" << windowClass;
        }

        if (nodeId != 0xFFFFFFFFu) {
          m_audioMixer->addSource(
              PipeWireAudioSource::createApplicationSource(nodeId, nodeName, nodeSerial),
              gameTrack);
          activeSources++;
          gameSourceAdded = true;
        } else {
          qWarning() << "No PipeWire node found for process" << gameProcess
                     << "/ class" << windowClass << "— game audio track will be silent";
        }
      }

      // === Voice chat (track 1 in separate; next contiguous slot in merged) ===
      vcApp = m_settings->voiceChatApp().trimmed();
      if (!vcApp.isEmpty()) {
        QString vcNodeName, vcSerial;
        uint32_t nodeId = PipeWireAudioSource::findNodeByProcessName(vcApp, &vcNodeName, &vcSerial);
        if (nodeId != 0xFFFFFFFF) {
          const int voiceTrack = isSeparate ? 1 : nextContiguous++;
          m_audioMixer->addSource(
              PipeWireAudioSource::createApplicationSource(nodeId, vcNodeName, vcSerial),
              voiceTrack);
          activeSources++;
          vcSourceAdded = true;
        } else {
          qWarning() << "No PipeWire node for voice chat app:" << vcApp;
        }
      }

      // === Microphone (track 2 in separate; next contiguous slot in merged) ===
      {
        const int micTrack = isSeparate ? 2 : nextContiguous++;
        m_audioMixer->addSource(PipeWireAudioSource::createInputSource(
            m_settings->microphoneDevice() == "default" ? QString()
                                                         : m_settings->microphoneDevice()), micTrack);
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

  // Create recording session to coordinate everything
  m_session = std::make_unique<RecordingSession>(
      m_currentFilePath, m_captureSource.get(), m_videoEncoder.get(),
      m_muxer.get(), m_audioMixer,
      m_settings->separateAudioTracks(),
      192,  // Audio bitrate: hard-coded to 192 kbps (CD quality)
      activeAudioTrackCount);

  // Set app names for periodic node checking during recording
  m_session->setGameApps(gameProcess, windowClass);
  m_session->setVoiceChatApp(vcApp);

#ifdef HAVE_PIPEWIRE
  // If sources were already found and added during buildPipeline, mark them as
  // connected so the node-check timer doesn't replace the already-running sources.
  if (gameSourceAdded) m_session->markSourceConnected(0);
  if (vcSourceAdded)   m_session->markSourceConnected(1);
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
  qInfo() << "Tearing down recording pipeline...";

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
  if (m_state == RecordingState::Recording ||
      m_state == RecordingState::Paused) {
    return m_recordingTimer.elapsed();
  }
  return 0;
}

qint64 RecordingManager::recordingSize() const {
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
  qCritical() << "Encoding error:" << error;
  emit recordingError(error);
  stopRecording();
}

} // namespace GameRecorder
