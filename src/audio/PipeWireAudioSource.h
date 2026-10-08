/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IAudioSource.h"
#include <QVariantList>
#include <atomic>
#include <memory>

namespace TrophyBackend {

/**
 * @brief PipeWire-based audio capture source
 *
 * Captures audio from desktop monitor, specific applications, or
 * microphone input devices using the native PipeWire API.
 */
class PipeWireAudioSource : public IAudioSource {
  Q_OBJECT

public:
  explicit PipeWireAudioSource(SourceType type, QObject *parent = nullptr);
  ~PipeWireAudioSource() override;

  // Factory methods
  static std::unique_ptr<PipeWireAudioSource>
  createMonitorSource(const QString &sinkName);
  /**
   * @brief Create an application audio capture source.
   * @param nodeId   PipeWire node ID of the application's playback stream.
   * @param nodeName PW_KEY_NODE_NAME of the node (used as target.object).
   * @param serial   object.serial of the node (preferred by WirePlumber 0.5).
   */
  static std::unique_ptr<PipeWireAudioSource>
  createApplicationSource(uint32_t nodeId,
                          const QString &nodeName = {},
                          const QString &serial = {});
  static std::unique_ptr<PipeWireAudioSource>
  createInputSource(const QString &deviceName);

  /**
   * @brief Enumerate PipeWire nodes to find one matching a process name.
   * @param processName  Name to search for (case-insensitive contains match).
   * @param outNodeName  Receives PW_KEY_NODE_NAME of the found node.
   * @param outSerial    Receives object.serial of the found node.
   * @return PipeWire node ID, or 0xffffffff if not found.
   */
  static uint32_t findNodeByProcessName(const QString &processName,
                                        QString *outNodeName = nullptr,
                                        QString *outSerial   = nullptr);

  /**
   * @brief Enumerate PipeWire nodes to find one matching a process ID.
   * @param pid         Process ID to search for.
   * @param outNodeName Receives PW_KEY_NODE_NAME of the found node.
   * @param outSerial   Receives object.serial of the found node.
   * @return PipeWire node ID, or 0xffffffff if not found.
   */
  static uint32_t findNodeByPid(uint32_t pid,
                                QString *outNodeName = nullptr,
                                QString *outSerial   = nullptr);

  /**
   * @brief Pick the target.object value for a pinned stream.
   * @return object.serial when known (unambiguous), else the node name
   * (resolved by name), else the numeric ID as a last resort.
   *
   * A bare numeric ID must never be preferred: WirePlumber reads
   * target.object as a *serial*, and IDs and serials share a namespace
   * (proven live: easyeffects_source id 73 / serial 74 vs easyeffects_sink
   * serial 73 — pinning "73" captured the wrong node). Static + public for
   * tests (no PipeWire needed).
   */
  static QString preferredTarget(const QString &serial,
                                 const QString &nodeName, uint32_t nodeId);

  /** Distinct stream name per track (shows up as the node name in qpwgraph:
   * "trophy-game", "trophy-voice", "trophy-mic"). */
  void setStreamName(const QString &name) {
    if (!name.isEmpty())
      m_streamName = name;
  }

  /**
   * @brief Enumerate all active audio applications.
   * @return List of QVariantMap objects with keys: "name", "binary", "nodeId"
   */
  static QVariantList enumerateAudioApps();

  bool initialize(const AudioConfig &config) override;
  void setCallback(AudioCallback callback) override;
  bool start() override;
  void stop() override;
  bool isCapturing() const override;
  SourceType sourceType() const override { return m_type; }
  QString displayName() const override { return m_displayName; }
  QString identifier() const override { return m_identifier; }
  float peakLevel() const override { return m_peakLevel; }
  void setVolume(float volume) override { m_volume.store(volume); }
  float volume() const override { return m_volume.load(); }

private:
  struct Private;
  std::unique_ptr<Private> d;

  SourceType m_type;
  QString m_displayName;
  QString m_identifier;
  QString m_streamName = QStringLiteral("trophy-capture");
  uint32_t m_targetNodeId;
  QString m_targetNodeName;  // PW_KEY_NODE_NAME for target.object hint
  QString m_targetSerial;    // object.serial for WirePlumber 0.5 target.object
  AudioConfig m_config;
  AudioCallback m_callback;
  std::atomic<bool> m_capturing{false};
  std::atomic<float> m_peakLevel{0.0f};
  std::atomic<float> m_volume{1.0f};
};

} // namespace TrophyBackend
