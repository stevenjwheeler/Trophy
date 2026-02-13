/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingManager.h"
#include "RecordingSession.h"
#include "StorageManager.h"
#include "audio/AudioMixer.h"

extern "C" {
#include <libavutil/pixfmt.h>
}
#include "capture/CaptureFactory.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/EncoderFactory.h"
#include "encoding/IEncoder.h"
#include "muxing/FFmpegMuxer.h"
#include "muxing/IMuxer.h"
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
      setState(RecordingState::Error);
      return;
    }
  }

  // Build the recording pipeline
  if (!buildPipeline()) {
    emit recordingError("Failed to initialize recording pipeline");
    setState(RecordingState::Error);
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

  QSize nativeRes = m_captureSource->nativeResolution();

  EncoderConfig encConfig;
  encConfig.width = nativeRes.width();
  encConfig.height = nativeRes.height();
  encConfig.fps = m_settings->fps();
  encConfig.bitrate = m_settings->videoBitrate();
  encConfig.preset = m_settings->encoderPreset();
  encConfig.inputPixelFormat = AV_PIX_FMT_BGRA;

  if (!m_videoEncoder->initialize(encConfig)) {
    qCritical() << "Failed to initialize video encoder";
    return false;
  }

  // Create recording session to coordinate everything
  m_session = std::make_unique<RecordingSession>(
      m_currentFilePath, m_captureSource.get(), m_videoEncoder.get(),
      m_muxer.get(), m_audioMixer);

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
  emit progressUpdated(recordingDuration(), recordingSize());
}

void RecordingManager::onEncodingError(const QString &error) {
  qCritical() << "Encoding error:" << error;
  emit recordingError(error);
  stopRecording();
  setState(RecordingState::Error);
}

} // namespace GameRecorder
