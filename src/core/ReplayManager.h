/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QJsonObject>
#include <cstdint>
#include <future>
#include <functional>
#include <atomic>
#include <vector>
#include <memory>
#include <thread>

#include "muxing/ReplayClipper.h"

namespace TrophyBackend {

class SettingsManager;
class GameDetectionService;
class StorageManager;
class AudioMixer;
class IVideoCaptureSource;
class IEncoder;
class IMuxer;
class RecordingSession;

/**
 * @brief Instant-replay DVR: rolling 60s segments while a game runs.
 *
 * While active, a full capture→encode→mux pipeline writes segments into
 * a cache dir (configurable, e.g. /tmp for RAM-backed storage), keeping
 * enough to cover the clip length plus one segment of margin. saveReplay
 * finalizes the current segment and stream-copies the trailing window
 * into the library — no re-encode, ready in seconds.
 *
 * Paused (and cleared) during real recordings; the DVR owns a dedicated
 * mixer so stopping it can't tear down the recording's audio sources.
 */
class ReplayManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool active READ isActive NOTIFY replayStateChanged)
  Q_PROPERTY(QString currentGame READ currentGame NOTIFY replayStateChanged)

public:
  static constexpr int kSegmentSeconds = 60;
  static constexpr int kPruneMarginSeconds = 60;
  static constexpr int kMinRetainedSegments = 2;

  explicit ReplayManager(QObject *parent = nullptr);
  ~ReplayManager() override;

  void setSettingsManager(SettingsManager *settings);
  void setGameDetectionService(GameDetectionService *detector) { m_gameDetector = detector; }
  void setStorageManager(StorageManager *storage);

  bool isActive() const { return m_state == State::Running; }
  bool isStarting() const { return m_state == State::StartingCapture; }
  QString currentGame() const { return m_gameName; }
  QVariantMap captureHealth() const;

  // Pure helpers (unit-tested): window/retention over segment lists.
  struct Segment {
    QString path;
    double startSec = 0.0; // since DVR start
    double durationSec = 0.0;
    QJsonObject audioHistory{};
    double endSec() const { return startSec + durationSec; }
  };
  static QList<ReplaySegment> selectForWindow(const QList<Segment> &segments,
                                              double nowSec, double windowSec);
  static QStringList pruneList(const QList<Segment> &segments, double nowSec,
                               double windowSec);

public slots:
  void startDvr(const QString &gameName);
  void stopDvr();
  // Paused during real recordings: stops the loop and clears the buffer.
  void setPaused(bool paused);
  void saveReplay();

signals:
  // The capture source is ready; portal selection no longer needs guidance.
  void captureSelectionFinished();
  void replayStateChanged();
  void replaySaved(const QString &filePath, double seconds);
  void replayFailed(const QString &reason);
  /**
   * The portal rejected our saved restore token. The app deletes it and
   * re-authorizes through the New Game prompt — never a bare picker.
   */
  void restoreAuthNeeded(const QString &gameName, const QString &identity = {});

private slots:
  void onCaptureReady();
  void onCaptureError(const QString &error);
  void onRestoreTokenStale(const QString &oldToken);
  void rotateSegment();

private:
  GameDetectionService *m_gameDetector = nullptr; // owner outlives this manager
  QString m_captureIdentity;
  enum class State { Idle, StartingCapture, Running, Stopping };

  void setState(State s);
  bool beginCapture();
  void connectCaptureSignals();
  // Gapless segment switch (rotation and clip saves): builds and starts
  // the successor session while the current one keeps recording, then
  // retires the old leg with a handoff stop. The old break-before-make
  // order dropped ~300ms of A/V on every switch (visible as a freeze in
  // any clip spanning the seam).
  bool switchSegment();
  bool retireOpenSegment(double switchSec);
  void pollRetirements();
  void completeDvrStop();
  void beginReplayRemux(double now);
  struct RetiringSegment {
    std::unique_ptr<RecordingSession> session;
    std::unique_ptr<IEncoder> encoder;
    std::unique_ptr<IMuxer> muxer;
    Segment segment;
  };
  std::vector<std::unique_ptr<RetiringSegment>> m_retiringSegments;
  QTimer *m_retirementTimer = nullptr;
  std::future<void> m_captureStopWorker;
  QString m_pendingGameName;
  bool m_shuttingDown = false;
  bool m_saveWaitingForRetirement = false;
  double m_saveRequestNow = 0.0;
  double m_saveRequestWindow = 0.0;
  QString m_saveRequestOutput;
  std::shared_ptr<std::atomic<bool>> m_saveDone;
  struct ReplaySaveResult {
    bool ok = false;
    QString path, error;
    double seconds = 0.0;
    QJsonObject audioHistory{};
  };
  std::shared_ptr<ReplaySaveResult> m_saveResult;
  QString audioSetupKey() const;
  void pruneOldSegments(double now);
  void clearSegments();
  QString cacheDir() const;
  // The pre-/tmp default (CacheLocation + "/trophy/replay"). Never written
  // anymore; cacheDir() removes it once so upgraders don't keep the stale
  // dir (it used to sit next to Qt's own caches as Trophy/trophy/replay).
  static QString legacyCacheDir();
  QString clipOutputPath() const;
  double elapsedSec() const;
  void setupAudioSources(int &trackCount, QStringList &gameHints,
                           uint32_t &gamePid, bool &gameFound, int &voiceTrack,
                           bool &voiceFound, int &micTrack,
                           bool createSources);
  // Pure PipeWire node lookup (no mixer/QObject state): PID first, then
   // name hints, then voice app. Worker-safe; all registry access runs in
   // an owned background preparation/cache operation.
  struct DvrNodeLookup {
    uint32_t gameNodeId = 0xFFFFFFFFu;
    QString gameNodeName;
    QString gameNodeSerial;
    bool gameFound = false;
    uint32_t voiceNodeId = 0xFFFFFFFFu;
    QString voiceNodeName;
    QString voiceNodeSerial;
    bool voiceFound = false;
  };
  static DvrNodeLookup lookupDvrNodes(uint32_t gamePid,
                                      const QStringList &gameHints,
                                      const QString &voiceApp);
  // Snapshot current inputs and refresh m_lookupCache on a worker for the
  // NEXT rotation. Never applies results to the live session (it runs its
  // own reconcile); stale deliveries are dropped by generation.
  void refreshLookupCacheAsync();
  void applyLookupCacheResult(quint64 generation, const DvrNodeLookup &fresh);
  void pollLookupWorker();
  std::future<DvrNodeLookup> m_lookupWorker;
  quint64 m_workerLookupGeneration = 0;
  QString m_workerAudioKey, m_lookupAudioKey;
  QPointer<IVideoCaptureSource> m_workerLookupCapture;
  QTimer *m_lookupTimer = nullptr;
  std::function<DvrNodeLookup(uint32_t, const QStringList &, const QString &)>
      m_nodeLookup = lookupDvrNodes;
  std::function<bool()> m_segmentBuilder;
  std::function<std::unique_ptr<IVideoCaptureSource>()> m_captureFactory;

  State m_state = State::Idle;
  QString m_gameName;
  bool m_paused = false;
  bool m_saving = false;
  std::thread m_saveThread;
  QStringList m_saveInputPaths;

 public:
  // Live audio retargeting, mirroring RecordingManager (acts on the open
  // segment's session; no-ops when the DVR is idle — the next segment
  // picks the new setting up).
  void applyMicrophoneDevice();
  void applyGameAudioDevice();
  void applyVoiceChatApp();

  // Mixer passthroughs for the Application-level mute proxy.
  void setTrackMuted(int trackIndex, bool muted);
  bool isTrackMuted(int trackIndex) const;
  float trackLevel(int trackIndex) const;
  void setSemanticTrackMuted(int role, bool muted);
  bool isSemanticTrackMuted(int role) const;
  float semanticTrackLevel(int role) const;

 private:
  // Mixer slots assigned per segment (needed to retarget mid-segment).
  int m_micTrack = -1;
  int m_voiceTrack = -1;
  // Audio configuration the live mixer sources were built from. While the
  // key matches, segment switches keep the running PipeWire streams and
  // only rotate encoders (gapless); a mismatch rebuilds sources.
  QString m_audioSetupKey;
   // Last node-lookup results. Retained-source rotations now take routing
    // directly from the current session (the cache cannot describe late-adopted
    // slots); startup awaits owned background preparation before publishing.
  DvrNodeLookup m_lookupCache;
  bool m_lookupCacheValid = false;
  quint64 m_lookupGeneration = 0;
  bool m_lookupInflight = false;
  bool m_captureReadyForPipeline = false;
  uint64_t m_captureGeneration = 0;

  // Test seam (same pattern as GameDetectionServiceTest).
  friend class ReplayManagerTest;

  SettingsManager *m_settings = nullptr;
  // Optional storage gate: when set, saveReplay refuses while the
  // library is over its limit (or when the clip would push it over),
  // instead of writing past the limit. Never deletes (unlike the
  // recording path's enforce call) — clips must not eat library files.
  StorageManager *m_storage = nullptr;
  // Dedicated mixer: sharing RecordingManager's mixer caused teardown
  // races (DVR stop ripped out the recording's just-added sources).
  // PipeWire happily serves both while they briefly overlap.
  std::unique_ptr<AudioMixer> m_audioMixer;

  std::unique_ptr<IVideoCaptureSource> m_capture;
  std::unique_ptr<IEncoder> m_encoder;
  std::unique_ptr<IMuxer> m_muxer;
  std::unique_ptr<RecordingSession> m_session;

  QList<Segment> m_segments;
  QElapsedTimer m_dvrClock;
  QTimer *m_rotationTimer = nullptr;
  int m_segmentIndex = 0;
  double m_openSegmentStart = 0.0;
  QString m_openSegmentPath;
};

} // namespace TrophyBackend
