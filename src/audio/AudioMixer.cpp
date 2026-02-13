/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioMixer.h"
#include "IAudioSource.h"

#include <QDebug>
#include <cmath>
#include <algorithm>

namespace GameRecorder {

AudioMixer::AudioMixer(int sampleRate, int channels, QObject *parent)
    : QObject(parent)
    , m_sampleRate(sampleRate)
    , m_channels(channels)
{
    // Initialize track configs (array is already default-constructed)
    m_tracks[0].config = {0, "Game Audio", 1.0f, false};
    m_tracks[1].config = {1, "Voice Chat", 1.0f, false};
    m_tracks[2].config = {2, "Microphone", 1.0f, false};

    qInfo() << "AudioMixer initialized:" << sampleRate << "Hz," << channels << "channels";
}

AudioMixer::~AudioMixer()
{
    stopAll();
}

void AudioMixer::addSource(std::unique_ptr<IAudioSource> source, int trackIndex)
{
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

        qInfo() << "Audio source added to track" << trackIndex
                << ":" << track.source->displayName();
    }
}

void AudioMixer::removeSource(int trackIndex)
{
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

void AudioMixer::setTrackVolume(int trackIndex, float volume)
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return;
    }

    m_tracks[trackIndex].config.volume = std::clamp(volume, 0.0f, 2.0f);
    emit trackConfigChanged(trackIndex);
}

void AudioMixer::setTrackMuted(int trackIndex, bool muted)
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return;
    }

    m_tracks[trackIndex].config.muted = muted;
    emit trackConfigChanged(trackIndex);
}

float AudioMixer::trackVolume(int trackIndex) const
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return 0.0f;
    }
    return m_tracks[trackIndex].config.volume;
}

bool AudioMixer::isTrackMuted(int trackIndex) const
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return true;
    }
    return m_tracks[trackIndex].config.muted;
}

QString AudioMixer::trackName(int trackIndex) const
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return QString();
    }
    return m_tracks[trackIndex].config.name;
}

float AudioMixer::getTrackLevel(int trackIndex) const
{
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= m_tracks.size()) {
        return 0.0f;
    }
    return m_tracks[trackIndex].peakLevel.load();
}

bool AudioMixer::startAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);

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

void AudioMixer::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto &track : m_tracks) {
        if (track.source) {
            track.source->stop();
        }
    }
}

void AudioMixer::onAudioReceived(int trackIndex, const float *samples, int frameCount)
{
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

    // Apply volume
    if (!track.config.muted) {
        peak *= track.config.volume;
    } else {
        peak = 0.0f;
    }

    // Smooth peak decay
    float currentPeak = track.peakLevel.load();
    if (peak > currentPeak) {
        track.peakLevel.store(peak);
    } else {
        // Decay at ~20dB/second
        float decay = 0.95f;
        track.peakLevel.store(currentPeak * decay);
    }

    // TODO: Store audio in buffer for encoder consumption
}

} // namespace GameRecorder
