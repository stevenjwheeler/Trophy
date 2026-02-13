/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>

namespace GameRecorder {

/**
 * @brief Audio frame data passed to callbacks
 */
struct AudioFrame {
    const float *samples;   // Interleaved float samples
    int frameCount;         // Number of frames (samples per channel)
    int channels;           // Number of channels
    int sampleRate;         // Sample rate in Hz
    int64_t timestamp;      // Presentation timestamp in microseconds
};

/**
 * @brief Configuration for audio capture
 */
struct AudioConfig {
    int sampleRate = 48000;
    int channels = 2;
    int bufferSize = 1024;  // Frames per buffer
};

/**
 * @brief Abstract interface for audio capture sources
 *
 * Implementations include PipeWire and PulseAudio sources for
 * capturing desktop audio, application audio, and microphone input.
 */
class IAudioSource : public QObject
{
    Q_OBJECT

public:
    enum class SourceType {
        Microphone,         // Input device (mic)
        ApplicationAudio,   // Specific application (e.g., Discord)
        DesktopAudio        // All desktop audio (monitor source)
    };
    Q_ENUM(SourceType)

    using AudioCallback = std::function<void(const AudioFrame &frame)>;

    explicit IAudioSource(QObject *parent = nullptr) : QObject(parent) {}
    ~IAudioSource() override = default;

    /**
     * @brief Initialize the audio source
     * @param config Audio configuration
     * @return true on success
     */
    virtual bool initialize(const AudioConfig &config) = 0;

    /**
     * @brief Set callback to receive audio frames
     * @param callback Function called for each audio buffer
     */
    virtual void setCallback(AudioCallback callback) = 0;

    /**
     * @brief Start capturing audio
     * @return true on success
     */
    virtual bool start() = 0;

    /**
     * @brief Stop capturing audio
     */
    virtual void stop() = 0;

    /**
     * @brief Check if currently capturing
     */
    virtual bool isCapturing() const = 0;

    /**
     * @brief Get the source type
     */
    virtual SourceType sourceType() const = 0;

    /**
     * @brief Get human-readable display name
     */
    virtual QString displayName() const = 0;

    /**
     * @brief Get device/source identifier
     */
    virtual QString identifier() const = 0;

    /**
     * @brief Get current peak level (0.0 - 1.0)
     */
    virtual float peakLevel() const = 0;

signals:
    void captureStarted();
    void captureStopped();
    void captureError(const QString &error);
    void levelChanged(float level);
};

} // namespace GameRecorder
