/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "EncodedStream.h"
#include "CaptureLifecycle.h"
#include <QObject>
#include <memory>
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <future>

namespace TrophyBackend {
class SettingsManager;
class GameDetectionService;
class AudioMixer;
class EnginePacketMuxer;
class RecordingSession;
class IEncoder;
class IMuxer;

// One demand-driven recording producer. Outputs are opaque encoded-packet
// consumers; no pipeline or native-resource handles cross this boundary.
class RecordingEngine : public QObject {
  Q_OBJECT
public:
  static constexpr int kReconfigurationSeconds = 60;
  enum class State { Idle, Starting, Running, Recovering, Stopping };
  Q_ENUM(State)
  enum class EndReason { Stopped, Handoff, PermissionClosed, Failed };
  Q_ENUM(EndReason)
  explicit RecordingEngine(QObject *parent = nullptr);
  ~RecordingEngine() override;
  void setSettingsManager(SettingsManager *settings);
  void setGameDetectionService(GameDetectionService *detector);
  void setAudioMixer(AudioMixer *mixer);
  quint64 attach(std::shared_ptr<EncodedOutput> output, const QString &game,
                 bool freshSelection = false);
  void detach(quint64 output);
  bool canReuseCapture(const QString &game) const;
  State state() const;
  bool hasDemand() const;
  bool requiresCaptureSelection() const;
  bool captureResumeRequired() const;
  QString currentGame() const;
  QVariantMap captureHealth() const;
  uint capturedSteamAppId() const;
  std::shared_ptr<CaptureLifecycle::Authorization> captureAuthorization() const;
  void setCaptureAuthorization(std::shared_ptr<CaptureLifecycle::Authorization> authorization);
  void applyMicrophoneDevice();
  void applyGameAudioDevice();
  void applyVoiceChatApp();
  void setSemanticTrackMuted(int track, bool muted);
  bool isSemanticTrackMuted(int track) const;
  float semanticTrackLevel(int track) const;
  void setTrackMuted(int track, bool muted);
  bool isTrackMuted(int track) const;
  float trackLevel(int track) const;
  QString activeEncoderName() const;
signals:
  void stateChanged();
  void captureSelectionFinished();
  void captureRecoveryChanged();
  void progressUpdated();
  void outputStarted(quint64 output);
  void generationEnded(quint64 output, bool succeeded, bool continuing, EndReason reason);
  void outputStopped(quint64 output, bool succeeded, const QString &reason);
  void outputError(quint64 output, const QString &reason);
  void restoreAuthNeeded(quint64 output, const QString &game, const QString &identity);
private:
  friend class EnginePacketMuxer;
  friend struct RecordingEngineTestAccess;
  friend class ApplicationAuthorizePromptTest;
  struct Shared;
  struct Private;
  std::shared_ptr<Shared> m_shared;
  std::unique_ptr<Private> m_private;
  void reconcile();
  void pollOutputs();
  void generationFinished(bool succeeded, bool continuing, const QJsonObject &history);
  void consumerFailed(quint64 id, const QString &reason);
  void closeOutput(quint64 id, bool succeeded = true, const QString &reason = {});
  // Native producer state belongs to this engine. The capture implementation
  // is split into a translation unit, not another orchestration object.
  friend class ApplicationAudioProbeTest;
  enum class RecordingState { Idle, Initializing, Recording, Stopping, Error };
  struct AudioPreparation {
    QString mode, gameDevice, micDevice, voiceApp;
    bool separate = true;
    uint32_t pid = 0;
    QStringList hints;
    uint32_t gameNode = 0xffffffffu, voiceNode = 0xffffffffu;
    QString gameNodeName, gameSerial, voiceNodeName, voiceSerial;
  };
  struct AudioRouting {
    int trackCount = 0, micTrack = -1, voiceTrack = -1;
    bool gameFound = false, voiceFound = false, gamePending = false;
  };
  Q_SIGNAL void nativeStateChanged();
  Q_SIGNAL void nativeStarted(const QString &label);
  Q_SIGNAL void nativeError(const QString &error);
  Q_SIGNAL void nativeProgress();
  void initializeProducer();
  void shutdownProducer();
  void configureCaptureSettings(SettingsManager *settings);
  void configureCaptureAudio(AudioMixer *mixer);
  void configureCaptureDetection(GameDetectionService *detector);
  void notifyRestoreAuth(const QString &game, const QString &identity);
  bool isRecording() const { return m_state == RecordingState::Recording; }
  bool isRecoveringCapture() const { return m_captureRecoveryPending; }
  QString currentGameName() const { return m_currentGameName; }
  void startRecording(const QString &gameName = {});
  void stopRecording();
  void setState(RecordingState state);
  bool buildPipeline();
  bool teardownPipeline();
  QString streamLabel() const;
  AudioPreparation snapshotAudioPreparation() const;
  static AudioPreparation lookupAudioPreparation(AudioPreparation input);
  static AudioPreparation snapshotAudio(const SettingsManager &settings,
      IVideoCaptureSource *capture, const QString &game, const QVariantMap &identity, bool manual);
  static AudioRouting configureAudio(AudioMixer *mixer, const AudioPreparation &input);
  std::unique_ptr<IEncoder> createEncoder();
  bool startCapture(bool windowOnly, const QString &restoreToken);
  void beginAudioPreparation();
  void completePipelineAndStart();
  void finishPipelineAndStart();
  void pollOperations();
  void finishStoppedRecording();
  void onCaptureError(const QString &error);
  void onEncodingError(const QString &error);
  void updateProgress();
  static bool isGenericSourceLabel(const QString &label);
  static bool sourceLabelsAgree(const QString &a, const QString &b);
  bool capturedGameHasEnded() const;
  bool recoveryInstanceIsCurrent() const;
  bool recoveryAwaitingCensus() const;
  void scheduleCaptureRecovery();
  void cancelCaptureRecovery(const QString &reason = QStringLiteral("cancelled"));
  bool interruptionIdentityIsCurrent() const;
  void clearCaptureInterruption();
  std::function<std::unique_ptr<IVideoCaptureSource>()> m_captureFactory;
  std::function<bool()> m_pipelineBuilder;
  std::function<std::unique_ptr<IMuxer>()> m_muxerFactory;
  QString m_captureGameName, m_pendingCaptureError;
  QElapsedTimer m_captureEndGrace;
  int m_captureEndGraceMs = 5000, m_recoveryDelayMs = 1500;
  bool m_captureRecoveryPending = false;
  CaptureLifecycle m_captureLifecycle;
  bool m_portalClosurePending = false;
  bool m_captureSelectionRequired = false, m_captureResumeRequired = false;
  bool m_forceCaptureSelectionOnce = false, m_explicitCaptureSelectionAttempt = false;
  QString m_interruptedGame, m_interruptedKey, m_interruptedInstance;
  bool m_stopNativeSettled = false;
  std::future<bool> m_stopFinalizationWorker;
  QString m_activeEncoderName;
  bool m_stopFinalizationSucceeded = true, m_cancelInitializing = false;
  QString m_pendingRestoreAuthorization, m_pendingRestoreIdentity;
  std::function<AudioPreparation(AudioPreparation)> m_audioPreparationLookup = lookupAudioPreparation;
  AudioPreparation m_preparedAudio;
  std::future<AudioPreparation> m_audioPreparationWorker;
  uint64_t m_pipelineGeneration = 0, m_audioPreparationGeneration = 0;
  bool m_audioPreparationRequested = false, m_captureReadyForPipeline = false;
  QPointer<IVideoCaptureSource> m_audioPreparationCapture;
  QTimer *m_operationTimer = nullptr, *m_progressTimer = nullptr;
  RecordingState m_state = RecordingState::Idle;
  QString m_currentGameName, m_streamLabel;
  QVariantMap m_detectedGameInfo;
  bool m_isManualRecording = false, m_recordingFailed = false, m_shuttingDown = false;
  QString m_lastOutputError;
  SettingsManager *m_settings = nullptr;
  AudioMixer *m_audioMixer = nullptr;
  GameDetectionService *m_gameDetector = nullptr;
  std::unique_ptr<RecordingSession> m_session;
  std::unique_ptr<IVideoCaptureSource> m_captureSource;
  std::unique_ptr<IEncoder> m_videoEncoder;
  std::unique_ptr<IMuxer> m_muxer;
  int m_micTrack = -1;
};
} // namespace TrophyBackend
