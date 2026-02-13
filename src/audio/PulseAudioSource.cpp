/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PulseAudioSource.h"
#include <QDebug>
#include <chrono>

namespace GameRecorder {

PulseAudioSource::PulseAudioSource(SourceType type, QObject *parent)
    : IAudioSource(parent), m_type(type) {}

PulseAudioSource::~PulseAudioSource() { stop(); }

std::unique_ptr<PulseAudioSource>
PulseAudioSource::createMonitorSource(const QString &sinkName) {
  auto source = std::make_unique<PulseAudioSource>(SourceType::DesktopAudio);
  source->m_identifier = sinkName + ".monitor";
  source->m_displayName = "Desktop Audio (PulseAudio)";
  return source;
}

std::unique_ptr<PulseAudioSource>
PulseAudioSource::createInputSource(const QString &deviceName) {
  auto source = std::make_unique<PulseAudioSource>(SourceType::Microphone);
  source->m_identifier = deviceName;
  source->m_displayName = deviceName.isEmpty() ? "Microphone" : deviceName;
  return source;
}

bool PulseAudioSource::initialize(const AudioConfig &config) {
  m_config = config;

  // TODO: Initialize PulseAudio connection using pa_simple
  qInfo() << "PulseAudio source initialized (stub):" << m_displayName;
  return true;
}

void PulseAudioSource::setCallback(AudioCallback callback) {
  m_callback = std::move(callback);
}

bool PulseAudioSource::start() {
  if (m_capturing) {
    return false;
  }

  m_capturing = true;
  m_captureThread = std::thread([this]() { captureLoop(); });

  emit captureStarted();
  qInfo() << "PulseAudio capture started:" << m_displayName;
  return true;
}

void PulseAudioSource::stop() {
  if (!m_capturing) {
    return;
  }

  m_capturing = false;
  if (m_captureThread.joinable()) {
    m_captureThread.join();
  }

  emit captureStopped();
  qInfo() << "PulseAudio capture stopped:" << m_displayName;
}

bool PulseAudioSource::isCapturing() const { return m_capturing; }

void PulseAudioSource::captureLoop() {
  auto interval = std::chrono::milliseconds(20);

  while (m_capturing) {
    // TODO: Actually capture audio from PulseAudio using pa_simple_read
    m_peakLevel = 0.0f;
    std::this_thread::sleep_for(interval);
  }
}

} // namespace GameRecorder
