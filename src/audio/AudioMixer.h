/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

#include "IAudioSource.h"

class QTimer;

namespace TrophyBackend {

class IAudioSource;

/**
 * @brief Multi-track audio mixer
 *
 * Manages multiple audio sources and provides separate tracks
 * for game audio, voice chat, and microphone.
 */
class AudioMixer : public QObject {
  Q_OBJECT

  Q_PROPERTY(int trackCount READ trackCount CONSTANT)

public:
  struct TrackConfig {
    int index;
    QString name;
    float volume = 1.0f;
    bool muted = false;
  };

  AudioMixer(int sampleRate, int channels, QObject *parent = nullptr);
  ~AudioMixer() override;

  int sampleRate() const { return m_sampleRate; }
  int channels() const { return m_channels; }
  static constexpr int kTrackCount = 3;
  int trackCount() const { return kTrackCount; }

  // Track management
  void addSource(std::unique_ptr<IAudioSource> source, int trackIndex);
  void removeSource(int trackIndex);
  // Owner-thread asynchronous variants. One native operation per track;
  // replacements retire immediately and queued starts coalesce to latest.
  void addSourceAsync(std::unique_ptr<IAudioSource> source, int trackIndex);
  void removeSourceAsync(int trackIndex);
  void ensureSourcesStartedAsync();
  void stopSourcesAsync();
  bool sourceOperationPending(int trackIndex) const;
  struct NodeLookupResult {
    uint32_t id = 0xffffffffu;
    QString name;
    QString serial;
  };
  // Lookup workers only own plain data, never a session. Retain discarded
  // session lookups until completion so rotation/destruction need not join
  // them on the GUI thread. The mixer joins any remaining work at teardown.
  void retainDiscardedNodeLookup(std::future<NodeLookupResult> lookup, int trackIndex);
  bool discardedNodeLookupPending(int trackIndex) const;
  // Snapshot source health; isCapturing() must be a nonblocking query.
  bool sourceIsCapturing(int trackIndex) const;

  // Track control
  Q_INVOKABLE void setTrackVolume(int trackIndex, float volume);
  Q_INVOKABLE void setTrackMuted(int trackIndex, bool muted);
  Q_INVOKABLE float trackVolume(int trackIndex) const;
  Q_INVOKABLE bool isTrackMuted(int trackIndex) const;
  Q_INVOKABLE QString trackName(int trackIndex) const;
  // Logical controls: Game/System=0, Voice=1, Mic=2. Preferences survive
  // absent sources and physical-slot changes (merged mic=1, late voice=2).
  Q_INVOKABLE void setSemanticTrackMuted(int role, bool muted);
  Q_INVOKABLE bool isSemanticTrackMuted(int role) const;
  Q_INVOKABLE float semanticTrackLevel(int role) const;

  // Audio level metering (0.0 - 1.0)
  Q_INVOKABLE float getTrackLevel(int trackIndex) const;

  /**
   * @brief Set a callback invoked with raw audio samples for each track.
   *
    * Called on the PipeWire capture thread. The callback receives:
    *   trackIndex, samples (interleaved float), frameCount, sampleRate, channels
    * Muted tracks deliver silence at their normal cadence, preserving time.
   */
  using TrackAudioCallback =
      std::function<void(int trackIndex, const float *samples, int frameCount,
                         int sampleRate, int channels)>;
  struct TrackAudioCallbackRegistration {
    explicit TrackAudioCallbackRegistration(TrackAudioCallback cb)
        : callback(std::move(cb)) {}
    TrackAudioCallback callback;
    std::mutex mutex;
    std::condition_variable drained;
    size_t inFlight = 0;
    bool accepting = true;
    // Diagnostic state, guarded by mutex (also observable by drain tests).
    bool drainTimedOut = false;
  };
  using TrackAudioCallbackHandle =
      std::shared_ptr<TrackAudioCallbackRegistration>;
  TrackAudioCallbackHandle setTrackAudioCallback(TrackAudioCallback callback);
  // Stop accepting callbacks through this registration and wait for any
  // callback already in flight. Does not change the currently published
  // registration, so it is safe to retire a session after its successor
  // has installed a different callback.
  // Must not be called from the callback being drained. Completion is a
  // lifetime barrier, not a timeout: callbacks must return promptly.
  void drainTrackAudioCallback(const TrackAudioCallbackHandle &handle);

  // Start/stop all sources
  // Retirement (replace/remove/stop/drain) waits for entered callbacks.
  // Do not invoke retirement from a source/output callback or its direct
  // signal receiver: that would wait on itself. Control stays on the owner
  // thread; overlapping starts/restarts or concurrent public-method calls
  // during destruction are not supported.
  bool startAll();
  // Start only sources that are not already capturing. DVR segment
  // switches keep PipeWire streams running across the handoff; blindly
  // restarting them would reopen a gap on every rotation.
  bool ensureSourcesStarted();
  void stopAll();

signals:
  void trackLevelChanged(int trackIndex, float level);
  void trackConfigChanged(int trackIndex);
  void semanticTrackMutedChanged(int role, bool muted);

private:
  friend class AudioMixerTestPeer;
  struct SourceCallbackRegistration {
    std::atomic<bool> accepting{true};
    std::mutex mutex;
    std::condition_variable drained;
    size_t inFlight = 0;
    bool draining = false; // Guarded by mutex; retirement diagnostic.
  };
  using SourceCallbackHandle = std::shared_ptr<SourceCallbackRegistration>;
  struct Track;
  void publishSourceCallback(Track &track);
  static int semanticRoleForSource(const IAudioSource &source, int trackIndex);
  void applySourceRole(Track &track);
  static void drainSourceCallback(const SourceCallbackHandle &handle);
  struct RetiredSource {
    std::shared_ptr<IAudioSource> source;
    SourceCallbackHandle callback;
  };
  struct SourceOperation {
    std::vector<RetiredSource> retired;
    std::shared_ptr<IAudioSource> start;
    uint64_t generation = 0;
    std::future<void> worker;
  };
  void replaceSourceAsync(std::unique_ptr<IAudioSource> source, int trackIndex);
  void pumpSourceOperations();
  void settleSourceOperations();

  struct Track {
    TrackConfig config;
    // Shared ownership: blocking PipeWire initialize()/start()/stop() run
    // on a retained copy WITHOUT holding m_mutex, so add/remove/stop never
    // contend with a slow start. m_mutex only publishes/unpublishes the slot
    // and bumps generation. Callback draining is outside this lock.
    // addSource keeps its unique_ptr boundary and converts to shared here,
    // so no caller signature changes.
    std::shared_ptr<IAudioSource> source;
    // A source callback may already have been copied by the audio thread
    // before setCallback(nullptr). Retire its independent token before
    // replacing the slot, so that late copy never enters the mixer.
    SourceCallbackHandle sourceCallbackActive;
    // Bumped on every publish/unpublish (addSource/removeSource) and on
    // stopAll/destructor detach. An in-flight start that finds a changed
    // generation (or a different published source) stops its retained copy
    // and drops it — never resurrecting a removed source. Guarded by
    // m_mutex.
    uint64_t generation = 0;
    int semanticRole = -1; // Owner-thread role; native callbacks use atomicMuted.
    std::vector<RetiredSource> pendingRetirements;
    std::unique_ptr<SourceOperation> operation;
    bool pendingStart = false;
    std::atomic<float> peakLevel{0.0f};
    // Atomic copies of config.volume and config.muted for lock-free reads
    // on the PipeWire callback thread (onAudioReceived).
    std::atomic<float> atomicVolume{1.0f};
    std::atomic<bool>  atomicMuted{false};
    // Last trackLevelChanged emit time (steady-clock microseconds). Read and
    // written on the PipeWire callback thread to decimate the signal to
    // ~10 Hz; the QML meters poll getTrackLevel() at 100 ms instead.
    std::atomic<int64_t> lastLevelEmitUs{0};
  };

  void onAudioReceived(int trackIndex, const float *samples, int frameCount,
                       const SourceCallbackHandle &sourceCallback);

  // Blocking initialize()+start() on a retained copy, then a generation
  // check under lock. Returns false on failure or when detached meanwhile
  // (the orphan is stopped and the published slot is left untouched).
  // Never holds m_mutex across PipeWire calls. Synchronous: the caller
  // must outlive the call, as with any QObject method (all call sites are
  // synchronous GUI-thread invocations; the PipeWire audio thread only
  // runs onAudioReceived; its registration lock is never held during calls).
  bool startRetainedSource(const std::shared_ptr<IAudioSource> &source,
                           int trackIndex, uint64_t generation,
                           const AudioConfig &config);

  int m_sampleRate;
  int m_channels;
  bool m_started = false;  // True after startAll() is called
  std::array<Track, kTrackCount> m_tracks;
  std::array<bool, kTrackCount> m_semanticMuted{};
  std::array<bool, kTrackCount> m_semanticMuteConfigured{};
  mutable std::mutex m_mutex;
  std::atomic<TrackAudioCallbackHandle> m_trackAudioCallback;
  QTimer *m_sourceOperationTimer = nullptr;
  struct DiscardedNodeLookup {
    int trackIndex;
    std::future<NodeLookupResult> worker;
  };
  std::vector<DiscardedNodeLookup> m_discardedNodeLookups;
};

} // namespace TrophyBackend
