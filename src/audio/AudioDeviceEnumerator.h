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
#include <QVariantList>

namespace TrophyBackend {

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
  Q_PROPERTY(QVariantList audioApps READ audioApps NOTIFY audioAppsChanged)

public:
  explicit AudioDeviceEnumerator(QObject *parent = nullptr);
  ~AudioDeviceEnumerator() override;

  QList<AudioDeviceInfo> inputDevices() const { return m_inputDevices; }
  QList<AudioDeviceInfo> outputDevices() const { return m_outputDevices; }
  QVariantList audioApps() const { return m_audioApps; }

  Q_INVOKABLE void refresh();
  Q_INVOKABLE void refreshAudioApps();

  Q_INVOKABLE QString defaultInputDevice() const;
  Q_INVOKABLE QString defaultOutputDevice() const;

signals:
  void devicesChanged();
  void audioAppsChanged();

private:
  void enumeratePipeWire();

  QList<AudioDeviceInfo> m_inputDevices;
  QList<AudioDeviceInfo> m_outputDevices;
  QVariantList m_audioApps;
};

} // namespace TrophyBackend

Q_DECLARE_METATYPE(TrophyBackend::AudioDeviceInfo)
