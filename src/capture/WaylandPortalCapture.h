/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IVideoCaptureSource.h"
#include <atomic>
#include <cstdint>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <vector>

namespace TrophyBackend {

/**
 * @brief Wayland screen capture using XDG Desktop Portal
 *
 * Uses the portal API to request screen sharing permission and
 * PipeWire to receive the video stream.
 */
class WaylandPortalCapture : public IVideoCaptureSource {
  Q_OBJECT

public:
  explicit WaylandPortalCapture(QObject *parent = nullptr);
  ~WaylandPortalCapture() override;

  bool initialize(const CaptureConfig &config) override;
  void setFrameCallback(FrameCallback callback) override;
  bool startCapture() override;
  void stopCapture() override;
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

private:
  // Issue SelectSources on the current session. Extracted so a stale
  // restore token can retry without it in the same session.
  void requestSelectSources();
  // Best-effort Session.Close for the portal handle. Every capture opens a
  // portal Session (which is what the tray "Screen sharing in progress"
  // indicator tracks); stopping PipeWire alone leaves the session — and
  // its indicator — alive forever.
  void closePortalSession();
  void startPipeWireStream(uint32_t nodeId);
  void stopPipeWireStream();

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

  pw_thread_loop *m_pw_loop = nullptr;
  pw_context *m_pw_context = nullptr;
  pw_core *m_pw_core = nullptr;
  pw_stream *m_pw_stream = nullptr;
  spa_hook m_pw_stream_listener;

  FrameCallback m_callback;
  CaptureConfig m_config;
  std::atomic<bool> m_capturing{false};
  QSize m_resolution;
  int m_pipewireNodeId = -1;
  QString m_sourceLabel;
  uint32_t m_sourcePid = 0;
  QString m_sourceAppId;
  bool m_useDmaBuf = true;        // Set from CaptureConfig
  bool m_dmaBufNegotiated = false; // Whether compositor accepted DMA-BUF
  uint64_t m_negotiatedModifier = 0; // DRM modifier agreed during format negotiation
  uint32_t m_negotiatedFormat = 0;   // DRM format agreed during format negotiation
  // Stage A: DMA-BUF modifiers queried from EGL for XRGB8888, offered to the
  // compositor tiled-first (LINEAR last). Empty = EGL unavailable, legacy
  // single modifier-agnostic offer is used.
  std::vector<uint64_t> m_offeredModifiers;
  bool m_buffersResultLogged = false;
  // Step-1 observability: per-buffer-type frame accounting for the
  // zero-copy vs. shared-memory paths. -1 = unknown, 0 = shm, 1 = DMA-BUF.
  uint64_t m_dmaBufFrames = 0;
  uint64_t m_shmFrames = 0;
  uint64_t m_droppedBuffers = 0;
  int m_lastBufferType = -1;
  bool m_multiPlaneWarned = false;
  bool m_gpuOnlyWarned = false;
};

} // namespace TrophyBackend
