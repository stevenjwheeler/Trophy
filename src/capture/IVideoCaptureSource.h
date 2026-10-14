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
#include <unistd.h> // ::dup(), ::close()
#include <vector>

namespace TrophyBackend {

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

  // DMA-BUF metadata
  bool isDmaBuf = false;
  int dmaBufFd = -1;
  uint32_t drmFormat = 0;
  uint64_t drmModifier = 0;
  int dmaBufStride = 0;
  int dmaBufOffset = 0;
};

/**
 * @brief Frame with owned data for queuing
 *
 * For DMA-BUF frames the pixel data lives in GPU memory (no CPU pointer).
 * We dup() the fd on construction so PipeWire can recycle its buffer as soon
 * as the capture callback returns.  The destructor closes the duped fd.
 * Copying is deleted to prevent double-close; use move semantics instead.
 */
struct QueuedFrame : public VideoFrame {
  std::vector<uint8_t> ownedData;

  explicit QueuedFrame(const VideoFrame &other) : VideoFrame(other) {
    if (!other.isDmaBuf && other.data && other.stride > 0 && other.height > 0) {
      ownedData.assign(other.data,
                       other.data + (size_t(other.stride) * other.height));
      data = ownedData.data();
    } else if (other.isDmaBuf && other.dmaBufFd >= 0) {
      // Dup so PipeWire can recycle its buffer immediately after our callback
      dmaBufFd = ::dup(other.dmaBufFd);
      if (dmaBufFd < 0)
        isDmaBuf = false; // dup failed – frame is unusable, drop silently
    }
  }

  // Move constructor – transfer fd ownership
  QueuedFrame(QueuedFrame &&other) noexcept
      : VideoFrame(static_cast<VideoFrame &&>(other)),
        ownedData(std::move(other.ownedData)) {
    if (!ownedData.empty())
      data = ownedData.data();
    other.dmaBufFd = -1;
  }

  // Move assignment – transfer fd ownership
  QueuedFrame &operator=(QueuedFrame &&other) noexcept {
    if (this != &other) {
      if (isDmaBuf && dmaBufFd >= 0)
        ::close(dmaBufFd);
      static_cast<VideoFrame &>(*this) = static_cast<VideoFrame &&>(other);
      ownedData = std::move(other.ownedData);
      if (!ownedData.empty())
        data = ownedData.data();
      other.dmaBufFd = -1;
    }
    return *this;
  }

  QueuedFrame(const QueuedFrame &) = delete;
  QueuedFrame &operator=(const QueuedFrame &) = delete;

  ~QueuedFrame() {
    if (isDmaBuf && dmaBufFd >= 0)
      ::close(dmaBufFd);
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
  bool useDmaBuf = true;
  QString waylandRestoreToken;
  // Game-targeted recordings (auto-detect) offer windows only in the
  // portal picker; manual recordings offer monitors + windows.
  bool windowOnly = false;
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
 * Implementations include WaylandPortalCapture.
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

  // Optional two-phase retirement. Call prepare on the QObject owner thread,
  // then finish on a worker while retaining the source and callback consumers.
  // Destroy/reuse only after finish returns, back on the owner thread. Do not
  // concurrently call stopCapture/destruct or launch multiple retirements.
  // Unsupported sources retain synchronous owner-thread stop compatibility.
  virtual void prepareStopCapture() { stopCapture(); }
  virtual void finishStopCapture() {}

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

  /**
   * @brief Emitted when a new Wayland restore token is received from the portal.
   * @param token The new restore token.
   */
signals:
  void captureStarted();
  void captureStopped();
  void captureError(const QString &error);
  void ready();

  /**
   * @brief Emitted when a new Wayland restore token is received from the portal.
   * @param token The new restore token.
   */
  void restoreTokenUpdated(const QString &token);

  /**
   * @brief Emitted when a saved restore token was rejected and capture
   * falls back to a fresh picker. The saved token is kept; a successful
   * Start overwrites it via restoreTokenUpdated.
   */
  void restoreTokenStale(const QString &oldToken);
};

} // namespace TrophyBackend
