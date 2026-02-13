/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IAudioSource.h"
#include <atomic>
#include <thread>

namespace GameRecorder {

/**
 * @brief PipeWire-based audio capture source
 *
 * Captures audio from desktop, applications, or input devices
 * using PipeWire.
 */
class PipeWireAudioSource : public IAudioSource {
  Q_OBJECT

public:
  explicit PipeWireAudioSource(SourceType type, QObject *parent = nullptr);
  ~PipeWireAudioSource() override;

  // Factory methods
  static std::unique_ptr<PipeWireAudioSource>
  createMonitorSource(const QString &sinkName);
  static std::unique_ptr<PipeWireAudioSource>
  createApplicationSource(uint32_t nodeId);
  static std::unique_ptr<PipeWireAudioSource>
  createInputSource(const QString &deviceName);

  bool initialize(const AudioConfig &config) override;
  void setCallback(AudioCallback callback) override;
  bool start() override;
  void stop() override;
  bool isCapturing() const override;
  SourceType sourceType() const override { return m_type; }
  QString displayName() const override { return m_displayName; }
  QString identifier() const override { return m_identifier; }
  float peakLevel() const override { return m_peakLevel; }

private:
  void captureLoop();

  SourceType m_type;
  QString m_displayName;
  QString m_identifier;
  AudioConfig m_config;
  AudioCallback m_callback;
  std::atomic<bool> m_capturing{false};
  std::atomic<float> m_peakLevel{0.0f};
  std::thread m_captureThread;
};

} // namespace GameRecorder
