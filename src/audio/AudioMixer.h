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
#include <memory>
#include <mutex>
#include <vector>

namespace GameRecorder {

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

  // Start/stop all sources
  bool startAll();
  void stopAll();

signals:
  void trackLevelChanged(int trackIndex, float level);
  void trackConfigChanged(int trackIndex);

private:
  struct Track {
    TrackConfig config;
    std::unique_ptr<IAudioSource> source;
    std::atomic<float> peakLevel{0.0f};
    std::vector<float> buffer;
  };

  void onAudioReceived(int trackIndex, const float *samples, int frameCount);

  int m_sampleRate;
  int m_channels;
  std::array<Track, kTrackCount> m_tracks;
  mutable std::mutex m_mutex;
};

} // namespace GameRecorder
