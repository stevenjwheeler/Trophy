/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IVideoCaptureSource.h"
#include <QDBusContext>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <vector>

class QDBusPendingCallWatcher;

namespace TrophyBackend {
class PipeWireBufferLease;

/**
 * @brief Wayland screen capture using XDG Desktop Portal
 *
 * Uses the portal API to request screen sharing permission and
 * PipeWire to receive the video stream.
 */
class WaylandPortalCapture : public IVideoCaptureSource, protected QDBusContext {
  Q_OBJECT

public:
  explicit WaylandPortalCapture(QObject *parent = nullptr);
  ~WaylandPortalCapture() override;

  bool initialize(const CaptureConfig &config) override;
  void setFrameCallback(FrameCallback callback) override;
  bool startCapture() override;
  void stopCapture() override;
  void prepareStopCapture() override;
  void finishStopCapture() override;
  bool isCapturing() const override;
  CaptureCapabilities capabilities() const override;
  QSize nativeResolution() const override;
  QString name() const override { return "Wayland Portal Capture"; }
  QString sourceLabel() const override { return m_sourceLabel; }
  uint32_t sourcePid() const override { return m_sourcePid; }
  QString sourceProcessName() const override { return m_sourceAppId; }

  Q_INVOKABLE void requestScreenShare();

signals:
  void screenShareGranted(const QString &pipewireNodeId);
  void screenShareDenied();

private slots:
  void onSessionCreated(unsigned int response, const QVariantMap &results);
  void onSourcesSelected(unsigned int response, const QVariantMap &results);
  void onStartResponse(unsigned int response, const QVariantMap &results);
  // Async portal method replies (GUI thread). Each validates the
  // request-handle reply and subscribes to the portal Request Response
  // signal, which arrives in the onSessionCreated/onSourcesSelected/
  // onStartResponse slot above. Late replies after watchdog/stop are
  // ignored via m_starting.
  void onCreateSessionCallFinished(QDBusPendingCallWatcher *watcher,
                                   quint64 generation);
  void onSelectSourcesCallFinished(QDBusPendingCallWatcher *watcher,
                                   quint64 generation);
  void onStartCallFinished(QDBusPendingCallWatcher *watcher,
                           quint64 generation);
  void onStartWatchdogTimeout();

private:
  friend class WaylandPortalCaptureTest;
  // Issue SelectSources on the current session. Extracted so a stale
  // restore token can retry without it in the same session.
  void requestSelectSources();
  // Best-effort Session.Close for the portal handle. Every capture opens a
  // portal Session (which is what the tray "Screen sharing in progress"
  // indicator tracks); stopping PipeWire alone leaves the session — and
  // its indicator — alive forever.
  void closePortalSession();
  bool startPipeWireStream(uint32_t nodeId);
  void stopPipeWireStream();
  void postNativeReady(quint64 generation);
  void postNativeError(const QString &error, quint64 generation);

  static void on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error);
  static void on_stream_param_changed(void *data, unsigned int id, const struct spa_pod *param);
  static void on_stream_process(void *data);

  QString m_sessionHandle;
  QString m_requestHandle;
  QString m_restoreToken;
  // True when this capture attempt started with a saved restore token.
  // Used to retry once without it if the portal rejects the token.
  bool m_restoreTokenUsed = false;
  QString m_pendingRequestPath;
  // Which portal leg the pending request belongs to. All three Response
  // slots share one permanent subscription (see
  // ensureResponseSubscription), so the request path alone cannot route
  // a signal: at create time the pending path IS the create path and
  // every slot's path check would pass. The stage picks the single
  // owning slot; the others drop without touching shared state.
  enum class PendingStage { None, Create, Select, Start };
  PendingStage m_pendingStage = PendingStage::None;
  // Permanent portal Request/Response subscription (see
  // ensureResponseSubscription): the daemon-side match rule must exist
  // before any method call goes out, otherwise a fast Response can
  // arrive before a per-reply connect() registers and is lost forever.
  bool m_responseSubscribed = false;
  void ensureResponseSubscription();

  pw_thread_loop *m_pw_loop = nullptr;
  pw_context *m_pw_context = nullptr;
  pw_core *m_pw_core = nullptr;
  pw_stream *m_pw_stream = nullptr;
  bool m_pw_loopStarted = false;
  spa_hook m_pw_stream_listener;

  FrameCallback m_callback;
  // Guards m_callback: setFrameCallback (GUI thread, incl. session
  // teardown) blocks until an in-flight on_stream_process delivery
  // finishes, so clearing it in stop() can never strand a running
  // invocation over a session that is about to be destroyed (SIGSEGV in
  // pthread_cond_signal on the freed queue condvar, seen live on DVR
  // rotation). Invoke holds the lock; callbacks only enqueue downstream.
  std::mutex m_callbackMutex;
  CaptureConfig m_config;
  std::atomic<bool> m_capturing{false};
  // prepare never waits on a callback/native lock. finish owns native teardown;
  // its final store re-enables owner-thread initialize/start after full drain.
  std::atomic_bool m_retiring{false};
  std::mutex m_retirementMutex;
  std::atomic_bool m_nativeEventsEnabled{false};
  std::atomic<quint64> m_nativeGeneration{0};
  // Private daemon-free teardown seam, configured only before retirement.
  std::function<void()> m_nativeStopOverride;
  // Published on the PipeWire stream thread (format negotiation), read on
  // the GUI thread by nativeResolution() and on the stream thread per
  // frame: separate atomics, no lock needed.
  std::atomic<int> m_resolutionWidth{0};
  std::atomic<int> m_resolutionHeight{0};
  int m_pipewireNodeId = -1;
  QString m_sourceLabel;
  uint32_t m_sourcePid = 0;
  QString m_sourceAppId;
  bool m_useDmaBuf = true;        // Set from CaptureConfig
  bool m_dmaBufNegotiated = false; // Whether compositor accepted DMA-BUF
  uint64_t m_negotiatedModifier = 0; // DRM modifier agreed during format negotiation
  uint32_t m_negotiatedFormat = 0;   // DRM format agreed during format negotiation
  // Stage A: DMA-BUF modifiers queried from EGL for XRGB8888, offered to the
  // compositor tiled-first (LINEAR last). Empty = shared memory only.
  std::vector<uint64_t> m_offeredModifiers;
  std::shared_ptr<PipeWireBufferLease> m_bufferLeases;
  bool m_buffersResultLogged = false;
  // Step-1 observability: per-buffer-type frame accounting for the
  // zero-copy vs. shared-memory paths. -1 = unknown, 0 = shm, 1 = DMA-BUF.
  // Written on the PipeWire stream thread, read on the GUI thread in
  // stopPipeWireStream(): atomics, no lock needed.
  std::atomic<uint64_t> m_dmaBufFrames{0};
  std::atomic<uint64_t> m_shmFrames{0};
  std::atomic<uint64_t> m_droppedBuffers{0};
  std::atomic<int> m_lastBufferType{-1};
  bool m_multiPlaneWarned = false;
  bool m_gpuOnlyWarned = false;
  // Format-negotiation dedup: ready() fires only on first negotiate or an
  // actual format change; a mid-stream renegotiation of the same format is
  // ignored so downstream encoder init is not re-fired. Touched only on
  // the PipeWire stream thread.
  bool m_formatReadyEmitted = false;
  uint32_t m_lastFormatVideo = 0;
  int m_lastFormatWidth = 0;
  int m_lastFormatHeight = 0;
  uint64_t m_lastFormatModifier = 0;
  // Start-up re-entry guard + watchdog: a concurrent start is rejected
  // while one portal round-trip is in flight; the single-shot watchdog
  // emits captureError if no terminal signal arrives in time. GUI thread
  // only. Method watchers capture their attempt generation, while response
  // slots verify the D-Bus request path. Comparing two mutable members
  // would let an old reply through after retry re-armed m_starting.
  bool m_starting = false;
  quint64 m_startGeneration = 0;
  quint64 m_attemptGeneration = 0;
  QTimer m_startWatchdog;
  void clearStartGuard();
  // Applies async PipeWire metadata enrichment (identity correction) back
  // on the GUI thread. Portal-provided labels are already usable
  // synchronously; this only refines them.
  void applySourceMetadata(const QString &name, uint32_t pid, bool found,
                           quint64 generation);
};

} // namespace TrophyBackend
