/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IAudioSource.h"
#include <QVariantList>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

struct spa_buffer;
struct spa_hook;
struct spa_pod;
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct pw_properties;
struct pw_stream_events;
struct pw_core_events;
struct pw_buffer;
class PipeWireAudioSourceTest;

namespace TrophyBackend {

namespace detail {

// Injectable bounded wait used by the synchronous registry helpers. Keeping
// iteration and completion predicates separate lets tests exercise timeout,
// completion, and iterator-error paths without a running PipeWire daemon.
template <typename Iterate, typename IsDone>
bool waitForPipeWireSync(Iterate &&iterate, IsDone &&isDone, int timeoutMs) {
  using Clock = std::chrono::steady_clock;
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!isDone()) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                              Clock::now())
            .count();
    if (remaining <= 0)
      return false;
    const int result = iterate(static_cast<int>(
        std::min<std::chrono::milliseconds::rep>(remaining, 100)));
    if (result < 0)
      return false;
  }
  return true;
}

} // namespace detail

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

  // Factories construct QObjects on the caller/owner thread. Deferred device
  // resolution performs its registry/default-device queries at initialize(),
  // for AudioMixer's retained native worker path; application factories never
  // perform registry queries.
  static std::unique_ptr<PipeWireAudioSource>
   createMonitorSource(const QString &sinkName, bool deferResolution = false);
  /**
   * @brief Create an application audio capture source.
   * @param nodeId   PipeWire node ID of the application's playback stream.
   * @param nodeName PW_KEY_NODE_NAME of the node (used as target.object).
   * @param serial   object.serial of the node (preferred by WirePlumber 0.5).
   */
  static std::unique_ptr<PipeWireAudioSource>
  createApplicationSource(uint32_t nodeId, const QString &nodeName = {},
                          const QString &serial = {});
  static std::unique_ptr<PipeWireAudioSource>
   createInputSource(const QString &deviceName, bool deferResolution = false);

  /**
   * @brief Enumerate PipeWire nodes to find one matching a process name.
   * @param processName  Name to search for (case-insensitive contains match).
   * @param outNodeName  Receives PW_KEY_NODE_NAME of the found node.
   * @param outSerial    Receives object.serial of the found node.
   * @return PipeWire node ID, or 0xffffffff if not found.
   */
  static uint32_t findNodeByProcessName(const QString &processName,
                                        QString *outNodeName = nullptr,
                                        QString *outSerial = nullptr);

  /**
   * @brief Enumerate PipeWire nodes to find one matching a process ID.
   * @param pid         Process ID to search for.
   * @param outNodeName Receives PW_KEY_NODE_NAME of the found node.
   * @param outSerial   Receives object.serial of the found node.
   * @return PipeWire node ID, or 0xffffffff if not found.
   */
  static uint32_t findNodeByPid(uint32_t pid, QString *outNodeName = nullptr,
                                QString *outSerial = nullptr);

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
  static QString preferredTarget(const QString &serial, const QString &nodeName,
                                 uint32_t nodeId);

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
  friend class ::PipeWireAudioSourceTest;
  void resolveDeviceTarget();
  bool m_deviceResolutionPending = false;
  // Per-instance lifecycle seam. Defaults call PipeWire; only the friend
  // fixture replaces them, before initialization (no runtime flags).
  struct Operations {
    void (*init)();
    pw_thread_loop *(*newLoop)();
    pw_context *(*newContext)(pw_thread_loop *);
    int (*startLoop)(pw_thread_loop *);
    void (*lockLoop)(pw_thread_loop *);
    void (*unlockLoop)(pw_thread_loop *);
    void (*stopLoop)(pw_thread_loop *);
    void (*destroyLoop)(pw_thread_loop *);
    pw_core *(*connectCore)(pw_context *);
    int (*disconnectCore)(pw_core *);
    void (*destroyContext)(pw_context *);
    pw_properties *(*newProperties)();
    int (*setProperty)(pw_properties *, const char *, const char *);
    void (*freeProperties)(pw_properties *);
    pw_stream *(*newStream)(pw_core *, const char *, pw_properties *);
    void (*addListener)(pw_stream *, spa_hook *, const pw_stream_events *, void *);
    int (*connectStream)(pw_stream *, uint32_t, int, const spa_pod *);
    void (*destroyStream)(pw_stream *);
    void (*addCoreListener)(pw_core *, spa_hook *, const pw_core_events *, void *);
    pw_buffer *(*dequeueBuffer)(pw_stream *);
    int (*queueBuffer)(pw_stream *, pw_buffer *);
    const pw_properties *(*getProperties)(pw_stream *);
  };
  static Operations realOperations();
  Operations m_operations = realOperations();
  // Mixer ownership keeps us alive but does not serialize start vs removal.
  // Callback publication/delivery deliberately never acquires this mutex.
  // Native listeners (including direct signal receivers) must not call
  // initialize/start/stop themselves: teardown waits for the loop callback.
  // Lifecycle actions from those receivers must be queued to the owner thread.
  std::mutex m_lifecycleMutex;
  bool stopLocked(); // Caller holds m_lifecycleMutex; emits no signals.
  void processBuffer(struct spa_buffer *buffer, int channels, int sampleRate);
  struct Private;
  std::unique_ptr<Private> d;

  SourceType m_type;
  QString m_displayName;
  QString m_identifier;
  QString m_streamName = QStringLiteral("trophy-capture");
  uint32_t m_targetNodeId;
  QString m_targetNodeName; // PW_KEY_NODE_NAME for target.object hint
  QString m_targetSerial;   // object.serial for WirePlumber 0.5 target.object
  AudioConfig m_config;
  // Published on the GUI thread (setCallback), loaded-and-copied on the
  // PipeWire thread before invoking — mirrors
  // AudioMixer::m_trackAudioCallback. Never read m_callback directly from
  // the audio thread; load the handle first.
  struct AudioCallbackRegistration {
    explicit AudioCallbackRegistration(AudioCallback cb)
        : callback(std::move(cb)) {}
    AudioCallback callback;
  };
  using AudioCallbackHandle = std::shared_ptr<AudioCallbackRegistration>;
  AudioCallbackHandle snapshotCallback() const {
    return m_callback.load(std::memory_order_acquire);
  }
  std::atomic<AudioCallbackHandle> m_callback;
  std::atomic<bool> m_capturing{false};
  std::atomic<float> m_peakLevel{0.0f};
  std::atomic<float> m_volume{1.0f};
};

} // namespace TrophyBackend
