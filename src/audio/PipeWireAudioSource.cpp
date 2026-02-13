/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PipeWireAudioSource.h"
#include <QDebug>
#include <chrono>

namespace GameRecorder {

PipeWireAudioSource::PipeWireAudioSource(SourceType type, QObject *parent)
    : IAudioSource(parent), m_type(type) {
  switch (type) {
  case SourceType::DesktopAudio:
    m_displayName = "Desktop Audio";
    break;
  case SourceType::ApplicationAudio:
    m_displayName = "Application Audio";
    break;
  case SourceType::Microphone:
    m_displayName = "Microphone";
    break;
  }
}

PipeWireAudioSource::~PipeWireAudioSource() { stop(); }

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createMonitorSource(const QString &sinkName) {
  auto source = std::make_unique<PipeWireAudioSource>(SourceType::DesktopAudio);
  source->m_identifier = sinkName;
  source->m_displayName = "Desktop Audio";
  return source;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createApplicationSource(uint32_t nodeId) {
  auto source =
      std::make_unique<PipeWireAudioSource>(SourceType::ApplicationAudio);
  source->m_identifier = QString::number(nodeId);
  return source;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createInputSource(const QString &deviceName) {
  auto source = std::make_unique<PipeWireAudioSource>(SourceType::Microphone);
  source->m_identifier = deviceName;
  source->m_displayName = deviceName.isEmpty() ? "Microphone" : deviceName;
  return source;
}

bool PipeWireAudioSource::initialize(const AudioConfig &config) {
  m_config = config;

  // TODO: Initialize PipeWire connection and stream
  qInfo() << "PipeWire audio source initialized (stub):" << m_displayName;
  return true;
}

void PipeWireAudioSource::setCallback(AudioCallback callback) {
  m_callback = std::move(callback);
}

bool PipeWireAudioSource::start() {
  if (m_capturing) {
    return false;
  }

  m_capturing = true;
  m_captureThread = std::thread([this]() { captureLoop(); });

  emit captureStarted();
  qInfo() << "PipeWire audio capture started:" << m_displayName;
  return true;
}

void PipeWireAudioSource::stop() {
  if (!m_capturing) {
    return;
  }

  m_capturing = false;
  if (m_captureThread.joinable()) {
    m_captureThread.join();
  }

  emit captureStopped();
  qInfo() << "PipeWire audio capture stopped:" << m_displayName;
}

bool PipeWireAudioSource::isCapturing() const { return m_capturing; }

void PipeWireAudioSource::captureLoop() {
  // Audio buffer interval (~20ms for 1024 samples at 48kHz)
  auto interval = std::chrono::milliseconds(20);

  while (m_capturing) {
    // TODO: Actually capture audio from PipeWire
    // For now, just simulate timing

    // Simulate some audio level activity
    m_peakLevel = 0.0f;

    std::this_thread::sleep_for(interval);
  }
}

} // namespace GameRecorder
