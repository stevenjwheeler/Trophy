/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <future>
#include <memory>

class RecordingManagerTest;

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
  Q_PROPERTY(QString detectedEncoderName READ detectedEncoderName NOTIFY detectedEncoderNameChanged)

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
  // Application's asynchronous availability result supplies this label.
  void setDetectedEncoderName(const QString &name);

  // Recording control
  Q_INVOKABLE void startRecording(const QString &gameName = QString(),
                                  unsigned long windowId = 0);
  Q_INVOKABLE void stopRecording();
  Q_INVOKABLE bool addBookmark();
  QVariantMap captureHealth() const;

  /**
   * @brief Whether a portal source label carries no real identity.
   *
   * Empty, compositor-generic (kwin/wayland/Unknown Game) and bare
   * portal app-ids (steam_app_<id> with no title behind them) say
   * nothing about what is actually captured.
   */
  static bool isGenericSourceLabel(const QString &label);
  /**
   * @brief Whether two source names plausibly denote the same target.
   *
   * Case-insensitive containment either way ("Deadlock" vs
   * "deadlock.exe"). Used to decide if the portal's captured source
   * agrees with the detected game before adopting it as the file name.
   */
  static bool sourceLabelsAgree(const QString &a, const QString &b);

signals:
  // The capture source is ready; portal selection no longer needs guidance.
  void captureSelectionFinished();
  void stateChanged(RecordingState state);
  void currentGameNameChanged();
  void recordingStarted(const QString &filePath);
  // Terminal cleanup for every active recording, including failed output.
  // recordingStopped below is the successful-save notification only.
  void recordingFinished();
  void recordingStopped(const QString &filePath, qint64 durationMs,
                        qint64 sizeBytes);
  void recordingError(const QString &error);
  /**
   * The portal rejected our saved restore token. The app deletes it and
   * re-authorizes through the New Game prompt — never a bare picker.
   */
  void restoreAuthNeeded(const QString &gameName, const QString &identity = {});
  void progressUpdated(qint64 duration, qint64 fileSize);
  void activeEncoderNameChanged();
  void detectedEncoderNameChanged();

private slots:
  void onEncodingError(const QString &error);
  void updateProgress();
  void finishPipelineAndStart();
  void pollOperations();

private:
  friend class ::RecordingManagerTest;
  friend class ApplicationAudioProbeTest;

  // Private injection seam for lifecycle tests; normal starts use CaptureFactory.
  std::function<std::unique_ptr<IVideoCaptureSource>()> m_captureFactory;
  std::function<bool()> m_pipelineBuilder;

  void setState(RecordingState newState);
  bool buildPipeline();
  bool teardownPipeline();
  QString generateOutputPath() const;
  struct AudioPreparation {
    QString mode, gameDevice, micDevice, voiceApp;
    bool separate = true;
    uint32_t pid = 0;
    QStringList hints;
    uint32_t gameNode = 0xffffffffu, voiceNode = 0xffffffffu;
    QString gameNodeName, gameSerial, voiceNodeName, voiceSerial;
  };
  AudioPreparation snapshotAudioPreparation() const;
  static AudioPreparation lookupAudioPreparation(AudioPreparation input);
  void beginAudioPreparation();
  void completePipelineAndStart();
  void finishStoppedRecording();
  std::future<bool> m_stopFinalizationWorker;
  qint64 m_stopDurationMs = 0;
  qint64 m_stopSizeBytes = 0;
  QString m_activeEncoderName;
  QString m_detectedEncoderName = QStringLiteral("Checking…");
  bool m_stopFinalizationSucceeded = true;
  bool m_cancelInitializing = false;
  QString m_pendingRestoreAuthorization;
  QString m_pendingRestoreIdentity;
  std::function<AudioPreparation(AudioPreparation)> m_audioPreparationLookup =
      lookupAudioPreparation;
  AudioPreparation m_preparedAudio;
  std::future<AudioPreparation> m_audioPreparationWorker;
  uint64_t m_pipelineGeneration = 0, m_audioPreparationGeneration = 0;
  bool m_audioPreparationRequested = false;
  bool m_captureReadyForPipeline = false;
  QPointer<IVideoCaptureSource> m_audioPreparationCapture;
  QTimer *m_operationTimer = nullptr;

  RecordingState m_state = RecordingState::Idle;
  QString m_currentGameName;
  QVariantMap m_detectedGameInfo;
  QString m_currentFilePath;
  unsigned long m_manualWindowId = 0;
  bool m_isManualRecording = false;
  bool m_recordingFailed = false;
  bool m_shuttingDown = false;

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
