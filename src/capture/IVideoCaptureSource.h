/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QSize>
#include <cstdint>
#include <functional>
#include <vector>

namespace GameRecorder {

/**
 * @brief Video frame data passed to callbacks
 */
struct VideoFrame {
  const uint8_t *data; // Frame pixel data
  int width;           // Frame width in pixels
  int height;          // Frame height in pixels
  int stride;          // Bytes per row (may include padding)
  int64_t timestamp;   // Presentation timestamp in microseconds
  int pixelFormat;     // FFmpeg AVPixelFormat value
};

/**
 * @brief Frame with owned data for queuing
 */
struct QueuedFrame : public VideoFrame {
  std::vector<uint8_t> ownedData;
  QueuedFrame(const VideoFrame &other) : VideoFrame(other) {
    ownedData.assign(other.data, other.data + (other.stride * other.height));
    data = ownedData.data();
  }
};

/**
 * @brief Configuration for video capture
 */
struct CaptureConfig {
  uint64_t windowId = 0; // 0 = capture whole screen/default
  int targetFps = 60;
  int maxWidth = 0;  // 0 = native resolution
  int maxHeight = 0; // 0 = native resolution
  bool captureMouseCursor = true;
  QString waylandRestoreToken;
};

/**
 * @brief Capabilities reported by capture source
 */
struct CaptureCapabilities {
  bool supportsWindowCapture = false;
  bool supportsMonitorCapture = false;
  bool supportsRegionCapture = false;
  bool supportsCursorCapture = false;
  bool supportsHardwareFrames = false; // e.g., DMA-BUF
  int maxFps = 60;
};

/**
 * @brief Abstract interface for video capture sources
 *
 * Implementations include X11WindowCapture and WaylandPortalCapture.
 * The capture source delivers frames via a callback mechanism.
 */
class IVideoCaptureSource : public QObject {
  Q_OBJECT

public:
  using FrameCallback = std::function<void(const VideoFrame &frame)>;

  explicit IVideoCaptureSource(QObject *parent = nullptr) : QObject(parent) {}
  ~IVideoCaptureSource() override = default;

  /**
   * @brief Initialize the capture source with given configuration
   * @param config Capture configuration
   * @return true on success
   */
  virtual bool initialize(const CaptureConfig &config) = 0;

  /**
   * @brief Set callback to receive captured frames
   * @param callback Function called for each captured frame
   */
  virtual void setFrameCallback(FrameCallback callback) = 0;

  /**
   * @brief Start capturing frames
   * @return true on success
   */
  virtual bool startCapture() = 0;

  /**
   * @brief Stop capturing frames
   */
  virtual void stopCapture() = 0;

  /**
   * @brief Check if currently capturing
   */
  virtual bool isCapturing() const = 0;

  /**
   * @brief Get capture capabilities
   */
  virtual CaptureCapabilities capabilities() const = 0;

  /**
   * @brief Get native resolution of capture target
   */
  virtual QSize nativeResolution() const = 0;

  /**
   * @brief Get human-readable name of the capture source
   */
  virtual QString name() const = 0;

  /**
   * @brief Get human-readable label of the selected content (e.g. window title)
   */
  virtual QString sourceLabel() const { return QString(); }

  /**
   * @brief Get PID of the captured application if available
   */
  virtual uint32_t sourcePid() const { return 0; }

  /**
   * @brief Get process name or ID of the captured application for audio routing
   * This can be an app_id (like org.mozilla.firefox) or a binary name.
   */
  virtual QString sourceProcessName() const { return QString(); }

signals:
  void captureStarted();
  void captureStopped();
  void captureError(const QString &error);
  void ready();
};

} // namespace GameRecorder
