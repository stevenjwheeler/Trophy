/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioDeviceEnumerator.h"
#include <QDebug>

namespace GameRecorder {

AudioDeviceEnumerator::AudioDeviceEnumerator(QObject *parent)
    : QObject(parent) {
  refresh();
}

AudioDeviceEnumerator::~AudioDeviceEnumerator() = default;

void AudioDeviceEnumerator::refresh() {
  m_inputDevices.clear();
  m_outputDevices.clear();

#ifdef HAVE_PIPEWIRE
  enumeratePipeWire();
#else
  enumeratePulseAudio();
#endif

  // Add default devices if none found
  if (m_inputDevices.isEmpty()) {
    AudioDeviceInfo defaultInput;
    defaultInput.id = "default";
    defaultInput.name = "Default Microphone";
    defaultInput.description = "System default input device";
    defaultInput.isInput = true;
    defaultInput.isDefault = true;
    m_inputDevices.append(defaultInput);
  }

  if (m_outputDevices.isEmpty()) {
    AudioDeviceInfo defaultOutput;
    defaultOutput.id = "default";
    defaultOutput.name = "Default Output";
    defaultOutput.description = "System default output device";
    defaultOutput.isInput = false;
    defaultOutput.isDefault = true;
    m_outputDevices.append(defaultOutput);
  }

  emit devicesChanged();
}

QString AudioDeviceEnumerator::defaultInputDevice() const {
  for (const auto &device : m_inputDevices) {
    if (device.isDefault) {
      return device.id;
    }
  }
  return m_inputDevices.isEmpty() ? "default" : m_inputDevices.first().id;
}

QString AudioDeviceEnumerator::defaultOutputDevice() const {
  for (const auto &device : m_outputDevices) {
    if (device.isDefault) {
      return device.id;
    }
  }
  return m_outputDevices.isEmpty() ? "default" : m_outputDevices.first().id;
}

void AudioDeviceEnumerator::enumeratePipeWire() {
  // TODO: Enumerate devices using PipeWire API
  qDebug() << "PipeWire device enumeration (stub)";
}

void AudioDeviceEnumerator::enumeratePulseAudio() {
  // TODO: Enumerate devices using PulseAudio API
  qDebug() << "PulseAudio device enumeration (stub)";
}

} // namespace GameRecorder
