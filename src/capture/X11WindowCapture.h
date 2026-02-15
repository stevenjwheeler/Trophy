/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IVideoCaptureSource.h"

#include <atomic>
#include <thread>

typedef void *XDisplayPtr;
typedef unsigned long XWindowId;
typedef void *XImagePtr;
typedef void *XShmSegmentInfoPtr;

namespace GameRecorder {

/**
 * @brief X11 window capture using MIT-SHM extension
 *
 * Captures frames from X11 windows using shared memory for performance.
 */
class X11WindowCapture : public IVideoCaptureSource {
  Q_OBJECT

public:
  explicit X11WindowCapture(QObject *parent = nullptr);
  ~X11WindowCapture() override;

  bool initialize(const CaptureConfig &config) override;
  void setFrameCallback(FrameCallback callback) override;
  bool startCapture() override;
  void stopCapture() override;
  bool isCapturing() const override;
  CaptureCapabilities capabilities() const override;
  QSize nativeResolution() const override;
  QString name() const override { return "X11 Window Capture"; }

private:
  void captureLoop();

  FrameCallback m_callback;
  CaptureConfig m_config;
  std::thread m_captureThread;
  std::atomic<bool> m_capturing{false};
  QSize m_resolution;

  XDisplayPtr m_display = nullptr;
  XWindowId m_targetWindow = 0;
  XImagePtr m_shmImage = nullptr;
  XShmSegmentInfoPtr m_shmInfo = nullptr;
  bool m_shmSupported = false;
};

} // namespace GameRecorder
