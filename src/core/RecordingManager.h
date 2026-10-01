/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <memory>

namespace TrophyBackend {

class SettingsManager;
class StorageManager;
class AudioMixer;
class WindowService;
class RecordingSession;
class IVideoCaptureSource;
class GameDetectionService;
class IEncoder;
class IMuxer;

/**
 * @brief Orchestrates the recording pipeline
 *
 * RecordingManager is responsible for:
 * - Managing recording state machine
 * - Creating and connecting capture, encoding, and muxing components
 * - Coordinating audio and video streams
 * - Handling recording lifecycle events
 */
class RecordingManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(RecordingState state READ state NOTIFY stateChanged)
  Q_PROPERTY(bool isRecording READ isRecording NOTIFY stateChanged)
  Q_PROPERTY(QString currentGameName READ currentGameName NOTIFY
                 currentGameNameChanged)
  Q_PROPERTY(
      qint64 recordingDuration READ recordingDuration NOTIFY progressUpdated)
  Q_PROPERTY(qint64 recordingSize READ recordingSize NOTIFY progressUpdated)
  Q_PROPERTY(
      QString currentFilePath READ currentFilePath NOTIFY recordingStarted)
  Q_PROPERTY(QString activeEncoderName READ activeEncoderName NOTIFY
                 activeEncoderNameChanged)
  Q_PROPERTY(QString detectedEncoderName READ detectedEncoderName CONSTANT)

public:
  enum class RecordingState {
    Idle,
    Initializing,
    Recording,
    Stopping,
    Error
  };
  Q_ENUM(RecordingState)

  explicit RecordingManager(QObject *parent = nullptr);
  ~RecordingManager() override;

  // Dependency injection
  void setSettingsManager(SettingsManager *settings);
  void setStorageManager(StorageManager *storage);
  void setAudioMixer(AudioMixer *mixer);
  void setWindowService(WindowService *ws);
  void setGameDetectionService(GameDetectionService *detector);

  // State
  RecordingState state() const { return m_state; }
  bool isRecording() const { return m_state == RecordingState::Recording; }
  QString currentGameName() const { return m_currentGameName; }
  qint64 recordingDuration() const; // milliseconds
  qint64 recordingSize() const;     // bytes
  QString currentFilePath() const { return m_currentFilePath; }
  QString activeEncoderName() const;
  QString detectedEncoderName() const;

  // Recording control
  Q_INVOKABLE void startRecording(const QString &gameName = QString(),
                                  unsigned long windowId = 0);
  Q_INVOKABLE void stopRecording();

signals:
  void stateChanged(RecordingState state);
  void currentGameNameChanged();
  void recordingStarted(const QString &filePath);
  void recordingStopped(const QString &filePath, qint64 durationMs,
                        qint64 sizeBytes);
  void recordingError(const QString &error);
  /**
   * The portal rejected our saved restore token. The app deletes it and
   * re-authorizes through the New Game prompt — never a bare picker.
   */
  void restoreAuthNeeded(const QString &gameName);
  void progressUpdated(qint64 duration, qint64 fileSize);
  void activeEncoderNameChanged();

private slots:
  void onEncodingError(const QString &error);
  void updateProgress();
  void finishPipelineAndStart();

private:
  void setState(RecordingState newState);
  bool buildPipeline();
  void teardownPipeline();
  QString generateOutputPath() const;

  RecordingState m_state = RecordingState::Idle;
  QString m_currentGameName;
  QString m_currentFilePath;
  unsigned long m_manualWindowId = 0;
  bool m_isManualRecording = false;

  // Dependencies (not owned)
  SettingsManager *m_settings = nullptr;
  StorageManager *m_storage = nullptr;
  AudioMixer *m_audioMixer = nullptr;
  WindowService *m_windowService = nullptr;
  GameDetectionService *m_gameDetector = nullptr;

  // Pipeline components (owned)
  std::unique_ptr<RecordingSession> m_session;
  std::unique_ptr<IVideoCaptureSource> m_captureSource;
  std::unique_ptr<IEncoder> m_videoEncoder;
  std::unique_ptr<IMuxer> m_muxer;

  // Progress tracking
  QElapsedTimer m_recordingTimer;
  QTimer *m_progressTimer = nullptr;

 public:
  // Live audio retargeting: swap one track's source on the running
  // pipeline (mixer hot-swaps; no-ops without an active session, in which
  // case the next build picks the new setting up). Invoked from
  // Application's settings-change wiring.
  void applyMicrophoneDevice();
  void applyGameAudioDevice();
  void applyVoiceChatApp();

 private:
  // Mixer slots assigned at build (needed to retarget mid-session).
  int m_micTrack = -1;
  int m_voiceTrack = -1;
};

} // namespace TrophyBackend
