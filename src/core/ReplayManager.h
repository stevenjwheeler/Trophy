/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "RecordingEngine.h"
#include <QTimer>
#include <future>
#include <QTemporaryDir>
#include <deque>
#include <mutex>

namespace TrophyBackend {
class SettingsManager;
class StorageManager;

// DVR policy, packet retention and export only. No native recording resources.
class ReplayManager : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ isActive NOTIFY replayStateChanged)
  Q_PROPERTY(bool recoveringCapture READ isRecoveringCapture NOTIFY replayStateChanged)
  Q_PROPERTY(QString currentGame READ currentGame NOTIFY replayStateChanged)
public:
  explicit ReplayManager(QObject *parent = nullptr);
  ReplayManager(std::shared_ptr<RecordingEngine> engine, QObject *parent = nullptr);
  ~ReplayManager() override;
  void setSettingsManager(SettingsManager *settings);
  void setStorageManager(StorageManager *storage) { m_storage = storage; }
  bool isActive() const { return m_id && m_started && !m_stopping; }
  bool isStarting() const { return m_id && !m_stopping && (!m_started || isRecoveringCapture()); }
  bool isRecoveringCapture() const {
    return m_id && m_engine->state() == RecordingEngine::State::Recovering;
  }
  QString currentGame() const { return m_game; }
  uint capturedSteamAppId() const { return m_engine->capturedSteamAppId(); }
  QVariantMap captureHealth() const;
public slots:
  void startDvr(const QString &game);
  bool selectCaptureWindow();
  void stopDvr();
  void setPaused(bool paused);
  void saveReplay();
signals:
  void captureSelectionFinished();
  void captureAuthorizationChanged();
  void replayStateChanged();
  void replaySaved(const QString &filePath, double seconds);
  void replayFailed(const QString &reason);
  void restoreAuthNeeded(const QString &gameName, const QString &identity = {});
private:
  friend class EncodedOutputsTest;
  struct Buffer final : EncodedOutput {
    struct Block {
      EncodedStream stream;
      QString path;
      qint64 startMs = 0, endMs = 0;
      QJsonObject history;
      std::shared_ptr<QTemporaryDir> directory;
      mutable std::mutex mutex;
      std::vector<EncodedFrame> packets;
      QVector<qint64> videoDtsMs;
      bool persisted = false;
      ~Block();
    };
    struct Piece {
      std::shared_ptr<Block> block;
      std::vector<EncodedFrame> packets;
      QVector<qint64> videoDtsMs;
    };
    struct Snapshot {
      QList<Piece> pieces;
      qint64 endMs = 0;
      int seconds = 30;
      QJsonObject history;
      bool save(const QString &path, QString *error, double *keptSeconds) const;
    };
    Buffer(QString cacheRoot, int seconds);
    ~Buffer() override;
    bool begin(const EncodedStream &stream) override;
    bool write(const EncodedFrame &packet) override;
    bool end() override;
    bool started() const override;
    QString error() const override;
    void setAudioHistory(quint64 generation, const QJsonObject &history) override;
    Snapshot snapshot(int seconds) const;
    QVariantMap health() const;
    void clear();
    static bool persist(const std::shared_ptr<Block> &block);
    static bool dump(const Block &block, const std::vector<EncodedFrame> &packets,
                     const QString &path, QString *error);
    void prune();
    mutable std::mutex m_mutex;
    std::deque<std::shared_ptr<Block>> m_blocks;
    std::shared_ptr<Block> m_current;
    std::shared_ptr<QTemporaryDir> m_directory;
    EncodedStream m_stream;
    int m_seconds = 30, m_sequence = 0;
    qint64 m_endMs = 0;
    QString m_error;
  };
  void start(const QString &game, bool fresh);
  void pollSave();
  struct SaveResult {
    bool ok = false;
    QString path, error;
    double seconds = 0;
  };
  std::shared_ptr<RecordingEngine> m_engine;
  std::shared_ptr<Buffer> m_buffer;
  SettingsManager *m_settings = nullptr;
  StorageManager *m_storage = nullptr;
  QString m_game, m_pendingGame;
  quint64 m_id = 0;
  bool m_paused = false, m_started = false, m_stopping = false, m_errorReported = false;
  QTimer m_saveTimer;
  std::future<SaveResult> m_save;
};
} // namespace TrophyBackend
