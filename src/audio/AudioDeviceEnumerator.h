/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QList>

namespace GameRecorder {

/**
 * @brief Audio device information
 */
struct AudioDeviceInfo {
    QString id;
    QString name;
    QString description;
    bool isInput;
    bool isDefault;
};

/**
 * @brief Enumerates available audio devices
 *
 * Discovers audio input and output devices using PipeWire or PulseAudio.
 */
class AudioDeviceEnumerator : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QList<AudioDeviceInfo> inputDevices READ inputDevices NOTIFY devicesChanged)
    Q_PROPERTY(QList<AudioDeviceInfo> outputDevices READ outputDevices NOTIFY devicesChanged)

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
