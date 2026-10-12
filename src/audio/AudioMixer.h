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
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

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

  // Track control
  Q_INVOKABLE void setTrackVolume(int trackIndex, float volume);
  Q_INVOKABLE void setTrackMuted(int trackIndex, bool muted);
  Q_INVOKABLE float trackVolume(int trackIndex) const;
  Q_INVOKABLE bool isTrackMuted(int trackIndex) const;
  Q_INVOKABLE QString trackName(int trackIndex) const;

  // Audio level metering (0.0 - 1.0)
  Q_INVOKABLE float getTrackLevel(int trackIndex) const;

  /**
   * @brief Set a callback invoked with raw audio samples for each track.
   *
   * Called on the PipeWire capture thread. The callback receives:
   *   trackIndex, samples (interleaved float), frameCount, sampleRate, channels
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
  };
  using TrackAudioCallbackHandle =
      std::shared_ptr<TrackAudioCallbackRegistration>;
  TrackAudioCallbackHandle setTrackAudioCallback(TrackAudioCallback callback);
  // Stop accepting callbacks through this registration and wait for any
  // callback already in flight. Does not change the currently published
  // registration, so it is safe to retire a session after its successor
  // has installed a different callback.
  void drainTrackAudioCallback(const TrackAudioCallbackHandle &handle);

  // Start/stop all sources
  bool startAll();
  // Start only sources that are not already capturing. DVR segment
  // switches keep PipeWire streams running across the handoff; blindly
  // restarting them would reopen a gap on every rotation.
  bool ensureSourcesStarted();
  void stopAll();

signals:
  void trackLevelChanged(int trackIndex, float level);
  void trackConfigChanged(int trackIndex);

private:
  struct Track {
    TrackConfig config;
    std::unique_ptr<IAudioSource> source;
    std::atomic<float> peakLevel{0.0f};
    // Atomic copies of config.volume and config.muted for lock-free reads
    // on the PipeWire callback thread (onAudioReceived).
    std::atomic<float> atomicVolume{1.0f};
    std::atomic<bool>  atomicMuted{false};
  };

  void onAudioReceived(int trackIndex, const float *samples, int frameCount);

  int m_sampleRate;
  int m_channels;
  bool m_started = false;  // True after startAll() is called
  std::array<Track, kTrackCount> m_tracks;
  mutable std::mutex m_mutex;
  std::atomic<TrackAudioCallbackHandle> m_trackAudioCallback;
};

} // namespace TrophyBackend
