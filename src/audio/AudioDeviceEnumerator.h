/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>

namespace GameRecorder {

/**
 * @brief Audio device information
 */
struct AudioDeviceInfo {
  Q_GADGET
  Q_PROPERTY(QString id MEMBER id)
  Q_PROPERTY(QString name MEMBER name)
  Q_PROPERTY(QString description MEMBER description)
  Q_PROPERTY(bool isInput MEMBER isInput)
  Q_PROPERTY(bool isDefault MEMBER isDefault)

public:
  QString id;
  QString name;
  QString description;
  bool isInput = false;
  bool isDefault = false;
};

/**
 * @brief Enumerates available audio devices
 *
 * Discovers audio input and output devices using PipeWire or PulseAudio.
 */
class AudioDeviceEnumerator : public QObject {
  Q_OBJECT

  Q_PROPERTY(QList<AudioDeviceInfo> inputDevices READ inputDevices NOTIFY
                 devicesChanged)
  Q_PROPERTY(QList<AudioDeviceInfo> outputDevices READ outputDevices NOTIFY
                 devicesChanged)

public:
  explicit AudioDeviceEnumerator(QObject *parent = nullptr);
  ~AudioDeviceEnumerator() override;

  QList<AudioDeviceInfo> inputDevices() const { return m_inputDevices; }
  QList<AudioDeviceInfo> outputDevices() const { return m_outputDevices; }

  Q_INVOKABLE void refresh();

  Q_INVOKABLE QString defaultInputDevice() const;
  Q_INVOKABLE QString defaultOutputDevice() const;

signals:
  void devicesChanged();

private:
  void enumeratePipeWire();
  void enumeratePulseAudio();

  QList<AudioDeviceInfo> m_inputDevices;
  QList<AudioDeviceInfo> m_outputDevices;
};

} // namespace GameRecorder

Q_DECLARE_METATYPE(GameRecorder::AudioDeviceInfo)
