/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "audio/AudioMixer.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/AudioEncoder.h"
#include "audio/AudioDriftCorrector.h"
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "FrameDispatcher.h"


namespace TrophyBackend {

class IVideoCaptureSource;
class IEncoder;
class IMuxer;

/**
 * @brief Manages an active recording session
 *
 * Coordinates capture, encoding, and muxing threads for a single recording.
 */
class RecordingSession : public QObject {
  Q_OBJECT

public:
  RecordingSession(const QString &outputPath, IVideoCaptureSource *capture,
                   IEncoder *encoder, IMuxer *muxer, AudioMixer *audioMixer,
                   bool separateAudioTracks, int audioBitrate,
                   int activeAudioTrackCount,
                   const QMap<QString, QString> &metadata = {},
                   QObject *parent = nullptr);
  ~RecordingSession() override;

  bool start();
  // keepCaptureSource skips stopping the capture + callbacks, so a new
  // session can attach to the still-running stream (DVR rotation).
  // handoffRouting (DVR make-before-break switch) additionally leaves the
  // live routing alone: the successor session already owns the capture
  // callback and the mixer callback/sources, so this session only drains
  // its queues, flushes its encoders and finalizes its file.
  void stop(bool keepCaptureSource = false, bool handoffRouting = false);
  // Owner-thread routing/timer retirement, followed by owned native drain,
  // encoding-thread join, flush and finalize. Owners retain all dependencies
  // until stopFinished. Direct stop() remains a synchronous lifetime barrier.
  void requestStop(bool keepCaptureSource = false, bool handoffRouting = false);
  bool stopPending() const { return m_stopWorker.valid(); }
  void retireAudioLookups();

  bool isRunning() const { return m_running; }
  // Packet callbacks only latch failure; owners inspect after stop() has
  // drained all producers. No reentrant teardown signal is emitted.
  bool outputFailed() const { return m_outputFailed.load(); }
  bool outputSucceeded() const {
    return m_outputFinalized.load() && !m_outputFailed.load();
  }

  qint64 frameCount() const { return m_frameCount; }
  qint64 bytesWritten() const { return m_bytesWritten; }

  // Owner-thread snapshots only: preserve the actual slot identities and
  // bindings over a DVR handoff, including voice adopted after initial setup.
  struct AudioRoutingState {
    int activeTrackCount;
    int voiceTrack;
    bool gameIsFallback;
    std::array<bool, AudioMixer::kTrackCount> connected;
    std::array<QString, AudioMixer::kTrackCount> boundSerial;
  };
  AudioRoutingState audioRoutingState() const;
  void inheritAudioRouting(const RecordingSession &previous);

  // Audio identities to watch for (called by RecordingManager/ReplayManager
  // during setup). Game takes a PID plus a list of name hints (window
  // process, portal app-id, detector display name — Proton nodes expose
  // only the exe basename, e.g. "deadlock.exe", with no PID/binary).
  void setGameApps(const QString &processName) {
    ++m_audioLookupGeneration;
    m_gamePid = 0;
    m_gameHints = processName.isEmpty() ? QStringList() : QStringList(processName);
  }
  void setGameAudioHints(uint32_t pid, const QStringList &hints) {
    ++m_audioLookupGeneration;
    m_gamePid = pid;
    m_gameHints = hints;
  }
  void setVoiceChatApp(const QString &appName) {
    ++m_audioLookupGeneration;
    m_voiceChatApp = appName;
  }
  // Mixer track reserved for voice (-1 when merged mode left it unslotted;
  // the retry adopts the next free slot instead of clobbering the mic).
  void setVoiceTrack(int track) { m_voiceTrack = track; }
  // Track 0 is on monitor fallback (full mix), not the real app node — the
  // retry keeps hunting instead of treating it as connected.
  void setGameIsFallback(bool fallback) { m_gameIsFallback = fallback; }

  void setFrameDispatcher(std::shared_ptr<TrophyBackend::FrameDispatcher> d) { m_frameDispatcher = d; }
  bool pushQueuedFrame(QueuedFrame &&frame);

  // Mark a track as already having a source so the node-check timer won't re-add it
  void markSourceConnected(int trackIndex) {
    if (trackIndex >= 0 && trackIndex < kAudioTrackCount)
      m_sourceConnected[trackIndex] = true;
  }
  // Forget a track's bind (used when the user retargets mid-session: the
  // old source is removed and the retry hunts the new identity).
  void forgetTrack(int trackIndex) {
    ++m_audioLookupGeneration;
    if (trackIndex >= 0 && trackIndex < kAudioTrackCount) {
      m_sourceConnected[trackIndex] = false;
      m_boundSerial[trackIndex].clear();
    }
  }

signals:
  void errorOccurred(const QString &error);
  void stopFinished();

private:
  friend class RecordingSessionTestPeer;
  void encodingLoop();
  bool retireStopRouting(bool keepCaptureSource, bool handoffRouting);
  void finishStopNative();
  void pollStopWorker();
  std::future<void> m_stopWorker;
  QTimer *m_stopTimer = nullptr;
  bool m_asyncHandoff = false;
  bool m_finishCaptureStop = false;
  bool writeOutputPacket(const EncodedPacket &packet);
  // Caller holds m_mergeMutex; silence fills missing slots after bounded grace.
  void drainMergedAudio(int channels, bool flushTail = false);
  void noteCapturedVideo(const VideoFrame &frame);
  bool advanceMergedClock(bool finalDrain = false);
  bool drainClockedMergedAudio(int channels, bool finalDrain);
  void appendClockedMergedAudio(int track, const float *samples, int frames,
                               int channels, int64_t arrivalUs);
  void applyMergedOffset();
  int64_t mergedVideoTargetFrames() const;
  int64_t mergedTimelineFrames(int64_t us) const;
  int64_t nowUs() const { return m_clockNowUs(); }
  std::function<int64_t()> m_clockNowUs = [] { return steadyNowMs() * 1000LL; };
  void checkForNewAudioNodes();
  void deliverAudioLookups();
  using AudioNodeResult = AudioMixer::NodeLookupResult;
  using AudioNodeLookup = std::function<AudioNodeResult(uint32_t, const QStringList &)>;
  static AudioNodeResult lookupAudioNode(uint32_t pid, const QStringList &hints);
  AudioNodeLookup m_audioNodeLookup = lookupAudioNode;
  struct AudioLookupOperation {
    std::future<AudioNodeResult> worker;
    uint64_t generation = 0;
    bool game = false;
  };
  std::array<AudioLookupOperation, AudioMixer::kTrackCount> m_audioLookups;
  uint64_t m_audioLookupGeneration = 0;
  QTimer *m_audioLookupTimer = nullptr;
  // Reconcile one app-capture track against the live registry: bind on
  // appearance, hot-swap when the stream generation changes, forget when
  // the node vanishes. isFallback (game track only) tracks monitor-fallback
  // state; it never counts as bound.
  void reconcileAppTrack(int track, uint32_t pid, const QStringList &hints,
                         bool *isFallback);
  void applyAppTrackNode(int track, uint32_t nodeId, const QString &nodeName,
                        const QString &nodeSerial, bool *isFallback);

  // Track index to muxer stream index: audio stream N = track N + 1
  static constexpr int kAudioTrackCount = 3;

  QString m_outputPath;
  IVideoCaptureSource *m_capture;
  IEncoder *m_encoder;
  IMuxer *m_muxer;
  AudioMixer *m_audioMixer;
  AudioMixer::TrackAudioCallbackHandle m_audioCallbackRegistration;

  // One encoder per audio track (game audio, voice chat, microphone)
  std::array<std::unique_ptr<AudioEncoder>, kAudioTrackCount> m_audioEncoders;
  std::array<std::mutex, kAudioTrackCount> m_audioEncoderMutexes;
  // Per-track wall-clock drift correctors (sound-card crystal vs video
  // clock). Invalid (SWR alloc failed) = pass-through.
  std::array<std::unique_ptr<AudioDriftCorrector>, kAudioTrackCount>
      m_driftCorrectors;

  // Merged audio track support
  bool m_separateAudioTracks = true;
  int m_audioBitrate = 192;
  // Written under m_mergeMutex (voice slot adoption) but read lock-free
  // on the audio callback thread — atomic so the callback never races
  // the GUI-thread retry path.
  std::atomic<int> m_activeTrackCount{0};
  std::unique_ptr<AudioEncoder> m_mergedEncoder;
  QMap<QString, QString> m_metadata;
  // Per-track accumulation buffers (merged mode) — different PipeWire sources
  // deliver different frame sizes, so each track accumulates independently.
  std::array<std::vector<float>, kAudioTrackCount> m_trackAccumulators;
  std::mutex m_mergeMutex;
  int64_t m_firstMergedInputWallUs = -1; // Guarded by m_mergeMutex.
  int64_t m_lastMergedInputWallUs = -1;
  std::atomic<int64_t> m_observedMergedInputWallUs{-1};
  int64_t m_mergedOffsetFrames = 0;
  int64_t m_mergedSubmittedFrames = 0;
  bool m_mergedClocked = false;
  // After >100ms without input, the owned encoding worker drives a continuous
  // merged timeline. Video timestamps anchor missing-input silence; observed
  // wall-clock input ends allow healthy audio to continue across video stalls.
  // Short cadence is trusted within 100ms, but timestampless late/backlogged
  // PCM outside the emitted cursor/8192-frame window is deliberately dropped.
  int64_t m_clockInputTargetFrames = 0;
  std::array<int64_t, kAudioTrackCount> m_clockBufferStartFrames{};
  std::array<int64_t, kAudioTrackCount> m_clockTrackNextFrames{};
  std::array<int64_t, kAudioTrackCount> m_clockTrackArrivalUs{};
  static constexpr int kMergedClockBlocksPerTick = 8;
  static constexpr int kMergedClockBufferedFrames = 8192;
  static constexpr int64_t kMergedClockGraceUs = 100000;
  std::atomic<int64_t> m_firstCapturedVideoTimestampUs{-1};
  std::atomic<int64_t> m_capturedVideoTimelineUs{-1};
  int64_t m_videoFrameDurationUs = 0; // Immutable after start, before worker creation.

  std::atomic<bool> m_running{false};
  std::atomic<bool> m_outputFailed{false};
  std::atomic<bool> m_outputFinalized{false};
  std::atomic<qint64> m_frameCount{0};
  std::atomic<qint64> m_bytesWritten{0};
  // A/V origin alignment: audio and video pipelines start at different
  // wall times yet both count timestamps from zero, baking in a constant
  // offset. Audio before the first video frame is dropped (unpairable
  // head content, 10s timeout force-open); tracks starting after it get
  // a one-time encoder start offset. First-wins latches (atomics: video
  // and audio run on different threads).
  std::atomic<int64_t> m_videoStartWallUs{-1};
  // First-wins origin latches. Written on the audio callback thread AND
  // the GUI-thread retry path (reconcileAppTrack resets a rebound track),
  // so plain bools raced — atomics.
  std::array<std::atomic<bool>, kAudioTrackCount> m_trackOffsetApplied{};
  std::atomic<bool> m_mergedOffsetApplied{false};
  static constexpr int64_t kVideoWaitTimeoutMs = 10000;
  static int64_t steadyNowMs();

  std::thread m_encodingThread;
  std::shared_ptr<TrophyBackend::FrameDispatcher> m_frameDispatcher;

  // Video frame queue for producer/consumer
  std::queue<QueuedFrame> m_videoQueue;
  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  const size_t MAX_QUEUE_SIZE = 30; // ~0.5s of buffer at 60fps

  // Audio node reconnection tracking
  QTimer *m_audioNodeCheckTimer = nullptr;
  uint32_t m_gamePid = 0;       // Game audio PID to watch for (0 = unknown)
  QStringList m_gameHints;      // Game name hints (exe, app-id, display name)
  QString m_voiceChatApp;       // Voice chat app name to watch for
  int m_voiceTrack = 1;         // Mixer slot reserved for voice (-1 = none)
  // Per-track bind state: bound serial of the node we're capturing (empty
  // when unbound). Track 0 additionally remembers monitor-fallback, which
  // never counts as bound — the retry keeps hunting the real app node.
  std::array<QString, kAudioTrackCount> m_boundSerial{};
  std::array<bool, kAudioTrackCount> m_sourceConnected{};
  // Session wall start (steady clock).
  int64_t m_sessionStartMs = -1;
  bool m_gameIsFallback = false;
  // One-shot note when voice capture is suppressed by full-mix fallback
  // (see checkForNewAudioNodes); re-arms if the game track ever leaves
  // fallback so a later suppression is still visible in the log.
  bool m_voiceFallbackNoteArmed = true;
  std::array<int, kAudioTrackCount> m_trackToStreamMap{};  // Map track index to muxer stream index
};

} // namespace TrophyBackend
