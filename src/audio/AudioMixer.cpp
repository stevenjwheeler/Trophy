/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioMixer.h"
#include "IAudioSource.h"

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <qlogging.h>

namespace GameRecorder {

AudioMixer::AudioMixer(int sampleRate, int channels, QObject *parent)
    : QObject(parent), m_sampleRate(sampleRate), m_channels(channels) {
  // Initialize track configs (array is already default-constructed)
  m_tracks[0].config = {0, "Game Audio", 1.0f, false};
  m_tracks[1].config = {1, "Voice Chat", 1.0f, false};
  m_tracks[2].config = {2, "Microphone", 1.0f, false};
  // Atomic copies start at defaults (1.0 volume, unmuted) — already set by
  // the in-class member initializers, but be explicit for clarity.
  for (auto &track : m_tracks) {
    track.atomicVolume.store(track.config.volume, std::memory_order_relaxed);
    track.atomicMuted.store(track.config.muted, std::memory_order_relaxed);
  }

  qDebug() << "AudioMixer initialized:" << sampleRate << "Hz," << channels
           << "channels";
}

AudioMixer::~AudioMixer() { stopAll(); }

void AudioMixer::addSource(std::unique_ptr<IAudioSource> source,
                           int trackIndex) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    qWarning() << "Invalid track index:" << trackIndex;
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);

  Track &track = m_tracks[trackIndex];

  // Stop existing source if any
  if (track.source) {
    track.source->stop();
  }

  track.source = std::move(source);

  if (track.source) {
    // Set up callback
    track.source->setCallback([this, trackIndex](const AudioFrame &frame) {
      onAudioReceived(trackIndex, frame.samples, frame.frameCount);
    });

    qDebug() << "Audio source added to track" << trackIndex << ":"
            << track.source->displayName();

    // If the mixer is already running (e.g., a source added mid-recording by
    // the node-check timer), initialize and start it immediately.
    if (m_started) {
      AudioConfig config;
      config.sampleRate = m_sampleRate;
      config.channels = m_channels;
      if (track.source->initialize(config)) {
        if (!track.source->start())
          qWarning() << "Failed to start late-added audio source on track"
                     << trackIndex;
      } else {
        qWarning() << "Failed to initialize late-added audio source on track"
                   << trackIndex;
      }
    }
  }
}

void AudioMixer::removeSource(int trackIndex) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);

  Track &track = m_tracks[trackIndex];
  if (track.source) {
    track.source->stop();
    track.source.reset();
  }
}

void AudioMixer::setTrackVolume(int trackIndex, float volume) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  float clamped = std::clamp(volume, 0.0f, 2.0f);
  m_tracks[trackIndex].config.volume = clamped;
  m_tracks[trackIndex].atomicVolume.store(clamped, std::memory_order_relaxed);
  emit trackConfigChanged(trackIndex);
}

void AudioMixer::setTrackMuted(int trackIndex, bool muted) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  m_tracks[trackIndex].config.muted = muted;
  m_tracks[trackIndex].atomicMuted.store(muted, std::memory_order_relaxed);
  emit trackConfigChanged(trackIndex);
}

float AudioMixer::trackVolume(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return 0.0f;
  }
  return m_tracks[trackIndex].config.volume;
}

bool AudioMixer::isTrackMuted(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return true;
  }
  return m_tracks[trackIndex].config.muted;
}

QString AudioMixer::trackName(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return QString();
  }
  return m_tracks[trackIndex].config.name;
}

float AudioMixer::getTrackLevel(int trackIndex) const {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return 0.0f;
  }
  return m_tracks[trackIndex].peakLevel.load();
}

bool AudioMixer::startAll() {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_started = true;
  bool allStarted = true;
  for (auto &track : m_tracks) {
    if (track.source) {
      AudioConfig config;
      config.sampleRate = m_sampleRate;
      config.channels = m_channels;

      if (!track.source->initialize(config)) {
        qWarning() << "Failed to initialize audio source for track"
                   << track.config.index;
        allStarted = false;
        continue;
      }

      if (!track.source->start()) {
        qWarning() << "Failed to start audio source for track"
                   << track.config.index;
        allStarted = false;
      }
    }
  }

  return allStarted;
}

void AudioMixer::stopAll() {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_started = false;
  for (auto &track : m_tracks) {
    if (track.source) {
      track.source->stop();
    }
  }
}

void AudioMixer::setTrackAudioCallback(TrackAudioCallback callback) {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_trackAudioCallback = std::move(callback);
}

void AudioMixer::onAudioReceived(int trackIndex, const float *samples,
                                 int frameCount) {
  if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
    return;
  }

  Track &track = m_tracks[trackIndex];

  // Calculate peak level for metering
  float peak = 0.0f;
  int totalSamples = frameCount * m_channels;

  for (int i = 0; i < totalSamples; i++) {
    float absVal = std::abs(samples[i]);
    if (absVal > peak) {
      peak = absVal;
    }
  }

  // Apply volume and mute — read from atomics for lock-free access on the
  // PipeWire callback thread (config fields are written from the UI thread).
  const bool muted = track.atomicMuted.load(std::memory_order_relaxed);
  const float volume = track.atomicVolume.load(std::memory_order_relaxed);
  if (!muted) {
    peak *= volume;
  } else {
    peak = 0.0f;
  }

  // Smooth peak decay
  float currentPeak = track.peakLevel.load();
  if (peak > currentPeak) {
    track.peakLevel.store(peak);
  } else {
    track.peakLevel.store(currentPeak * 0.95f);
  }

  // Emit level change signal for UI updates
  emit trackLevelChanged(trackIndex, track.peakLevel.load());

  // Forward audio to encoder callback (called on PipeWire thread)
  if (!muted && m_trackAudioCallback) {
    m_trackAudioCallback(trackIndex, samples, frameCount, m_sampleRate,
                         m_channels);
  }
}

} // namespace GameRecorder
