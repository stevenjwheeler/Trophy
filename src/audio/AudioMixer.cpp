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

namespace TrophyBackend {

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

bool AudioMixer::ensureSourcesStarted() {
  std::lock_guard<std::mutex> lock(m_mutex);

  m_started = true;
  bool allStarted = true;
  for (auto &track : m_tracks) {
    if (track.source && !track.source->isCapturing()) {
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

AudioMixer::TrackAudioCallbackHandle
AudioMixer::setTrackAudioCallback(TrackAudioCallback callback) {
  TrackAudioCallbackHandle published;
  if (callback)
    published = std::make_shared<TrackAudioCallbackRegistration>(
        std::move(callback));
  m_trackAudioCallback.store(published, std::memory_order_release);
  return published;
}

void AudioMixer::drainTrackAudioCallback(
    const TrackAudioCallbackHandle &handle) {
  if (!handle)
    return;
  std::unique_lock<std::mutex> lock(handle->mutex);
  handle->accepting = false;
  handle->drained.wait(lock, [&] { return handle->inFlight == 0; });
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
  if (muted) {
    // Muted tracks read flat zero so meters park immediately.
    track.peakLevel.store(0.0f);
  } else {
    peak *= volume;
    // Perceptual mapping: a linear peak parks typical loud content
    // (-20dB = 0.1) at a 10% sliver. Map -60..0dB onto 0..1 so -20dB
    // reads ~0.67 and -12dB ~0.8, while quiet content stays low.
    // Result stays in 0..1 (hotter-than-full-scale clamps at 1).
    float display = 0.0f;
    if (peak > 0.0f) {
      const float dB = 20.0f * std::log10(peak);
      display = std::clamp((dB + 60.0f) / 60.0f, 0.0f, 1.0f);
    }

    // Fast attack, smooth release. Release runs in display space, so the
    // bar falls linearly instead of diving logarithmically.
    const float currentPeak = track.peakLevel.load();
    track.peakLevel.store(display > currentPeak ? display
                                               : currentPeak * 0.95f);
  }

  // Emit level change signal for UI updates
  emit trackLevelChanged(trackIndex, track.peakLevel.load());

  // Forward audio to encoder callback (called on PipeWire thread)
  const auto callback = m_trackAudioCallback.load(std::memory_order_acquire);
  if (muted || !callback)
    return;
  {
    std::lock_guard<std::mutex> lock(callback->mutex);
    if (!callback->accepting)
      return;
    ++callback->inFlight;
  }
  struct CallbackCompletion {
    TrackAudioCallbackHandle registration;
    ~CallbackCompletion() {
      std::lock_guard<std::mutex> lock(registration->mutex);
      if (--registration->inFlight == 0)
        registration->drained.notify_all();
    }
  } completion{callback};
  callback->callback(trackIndex, samples, frameCount, m_sampleRate, m_channels);
}

} // namespace TrophyBackend
