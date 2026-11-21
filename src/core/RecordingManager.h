/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "RecordingEngine.h"
#include <QObject>
#include <atomic>
#include <mutex>

namespace TrophyBackend {
class SettingsManager;
class StorageManager;

// Full-recording policy and library feedback. Native recording work belongs
// exclusively to RecordingEngine; this facade owns only a packet file output.
class RecordingManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(RecordingState state READ state NOTIFY stateChanged)
  Q_PROPERTY(bool isRecording READ isRecording NOTIFY stateChanged)
  Q_PROPERTY(bool captureResumeRequired READ captureResumeRequired NOTIFY captureRecoveryChanged)
  Q_PROPERTY(bool requiresCaptureSelection READ requiresCaptureSelection NOTIFY captureRecoveryChanged)
  Q_PROPERTY(QString currentGameName READ currentGameName NOTIFY currentGameNameChanged)
  Q_PROPERTY(qint64 recordingDuration READ recordingDuration NOTIFY progressUpdated)
  Q_PROPERTY(qint64 recordingSize READ recordingSize NOTIFY progressUpdated)
  Q_PROPERTY(QString currentFilePath READ currentFilePath NOTIFY recordingStarted)
  Q_PROPERTY(QString activeEncoderName READ activeEncoderName NOTIFY activeEncoderNameChanged)
  Q_PROPERTY(QString detectedEncoderName READ detectedEncoderName NOTIFY detectedEncoderNameChanged)
public:
  enum class RecordingState { Idle, Initializing, Recording, Stopping, Error };
  Q_ENUM(RecordingState)
  explicit RecordingManager(QObject *parent = nullptr);
  RecordingManager(std::shared_ptr<RecordingEngine> engine, QObject *parent = nullptr);
  ~RecordingManager() override;
  void setSettingsManager(SettingsManager *settings);
  void setStorageManager(StorageManager *storage) { m_storage = storage; }
  RecordingState state() const { return m_state; }
  bool isRecording() const { return m_state == RecordingState::Recording; }
  bool isRecoveringCapture() const;
  bool captureResumeRequired() const { return m_engine->captureResumeRequired(); }
  bool requiresCaptureSelection() const { return m_engine->requiresCaptureSelection(); }
  bool lastRecordingFailed() const { return m_failed; }
  QString currentGameName() const { return m_game; }
  uint capturedSteamAppId() const { return m_engine->capturedSteamAppId(); }
  qint64 recordingDuration() const;
  qint64 recordingSize() const;
  QString currentFilePath() const;
  QString activeEncoderName() const { return m_engine->activeEncoderName(); }
  QString detectedEncoderName() const { return m_detectedEncoder; }
  void setDetectedEncoderName(const QString &name);
  std::shared_ptr<CaptureLifecycle::Authorization> captureAuthorization() const {
    return m_engine->captureAuthorization();
  }
  QVariantMap captureHealth() const { return m_engine->captureHealth(); }
  Q_INVOKABLE void startRecording(const QString &game = {});
  Q_INVOKABLE void stopRecording();
  Q_INVOKABLE bool resumeCapture();
  Q_INVOKABLE bool addBookmark();
signals:
  void captureSelectionFinished();
  void stateChanged(RecordingState state);
  void currentGameNameChanged();
  void recordingStarted(const QString &filePath);
  void recordingPathPrepared(const QString &filePath);
  void recordingFinished();
  void recordingStopping();
  void captureRecoveryChanged();
  void captureResumeOffered(const QString &gameName);
  void recordingStopped(const QString &filePath, qint64 durationMs, qint64 sizeBytes, bool quiet = false);
  void recordingCompletionReady(const QString &filePath, qint64 durationMs, qint64 sizeBytes);
  void recordingError(const QString &error);
  void restoreAuthNeeded(const QString &gameName, const QString &identity = {});
  void progressUpdated(qint64 duration, qint64 fileSize);
  void activeEncoderNameChanged();
  void detectedEncoderNameChanged();
private:
  friend class ApplicationAuthorizePromptTest;
  friend class ApplicationAudioProbeTest;
  friend class EncodedOutputsTest;
  friend class RecordingManagerTest;
  friend struct RecordingEngineTestAccess;
  struct FileOutput final : EncodedOutput {
    struct Part {
      quint64 generation = 0;
      QString path;
      qint64 durationMs = 0, bytes = 0;
      bool succeeded = false;
      QJsonObject history;
    };
    explicit FileOutput(QString directory);
    ~FileOutput() override;
    bool begin(const EncodedStream &stream) override;
    bool write(const EncodedFrame &packet) override;
    bool end() override;
    bool started() const override { return m_started.load(); }
    QString error() const override;
    void setAudioHistory(quint64 generation, const QJsonObject &history) override;
    QString path() const;
    qint64 durationMs() const { return m_duration.load(); }
    qint64 positionMs() const { return m_position.load(); }
    qint64 bytes() const { return m_bytes.load(); }
    QList<Part> takeParts();
    bool open();
    QString preparePath(const QString &game);
    QString m_directory;
    EncodedStream m_stream;
    std::shared_ptr<IMuxer> m_muxer;
    mutable std::mutex m_mutex;
    QString m_path, m_nextPath, m_error;
    QList<Part> m_parts;
    qint64 m_origin = 0;
    bool m_open = false;
    std::atomic<bool> m_started{false};
    std::atomic<qint64> m_duration{0}, m_bytes{0}, m_position{-1};
  };
  void start(const QString &game, bool fresh);
  void setState(RecordingState state);
  void fileParts(bool quiet);
  void preparePart();
  void completeFeedback();
  std::shared_ptr<RecordingEngine> m_engine;
  std::shared_ptr<FileOutput> m_output;
  SettingsManager *m_settings = nullptr;
  StorageManager *m_storage = nullptr;
  quint64 m_id = 0;
  QString m_game, m_detectedEncoder = QStringLiteral("Checking…");
  RecordingState m_state = RecordingState::Idle;
  FileOutput::Part m_deferred;
  bool m_failed = false, m_errorReported = false, m_quietStop = false;
};
} // namespace TrophyBackend
