/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "DmaBufFrame.h"
#include <QObject>
#include <QSize>
#include <cstdint>
#include <climits>
#include <functional>
#include <memory>
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
  DmaBufLayout dmaBufPlanes; // Borrowed unless dmaBufStorage is populated.
  std::shared_ptr<const OwnedDmaBuf> dmaBufStorage;
  std::shared_ptr<void> bufferLease; // delays producer reuse, separate from fd ownership
  std::function<void()> requestCpuFallback;
  // Optional immutable CPU snapshot, independent of the producer's mapping.
  std::shared_ptr<const std::vector<uint8_t>> cpuStorage;

  DmaBufLayout dmaBufLayout() const {
    if (dmaBufStorage) return dmaBufStorage->layout();
    if (dmaBufPlanes.count) return dmaBufPlanes;
    DmaBufLayout result;
    result.count = 1;
    result.planes[0] = {dmaBufFd, dmaBufStride, uint32_t(dmaBufOffset), 0};
    return result;
  }
  bool retainDmaBuf() {
    if (!isDmaBuf) return false;
    const auto layout = dmaBufLayout();
    if (!layout.count || layout.count > DmaBufLayout::maxPlanes ||
        layout.planes[0].offset > uint32_t(INT_MAX))
      return false; // Legacy primary-offset view is signed.
    if (!dmaBufStorage) {
      if (!dmaBufPlanes.count && dmaBufOffset < 0) return false;
      dmaBufStorage = OwnedDmaBuf::retain(dmaBufLayout(), bufferLease);
    }
    if (!dmaBufStorage) return false;
    // Legacy fields are views of plane zero, never additional owned handles.
    dmaBufPlanes = dmaBufStorage->layout();
    const auto &primary = dmaBufPlanes.planes[0];
    dmaBufFd = primary.fd;
    dmaBufStride = primary.stride;
    dmaBufOffset = int(primary.offset);
    return true;
  }
};

/**
 * @brief Frame with owned data for queuing
 *
 * For DMA-BUF frames the pixel data lives in GPU memory (no CPU pointer).
 * Borrowed handles are duplicated once into a shared immutable owner; queued
 * and retained still frames share that owner and the producer buffer lease.
 * The last consumer closes every plane FD and releases the lease.
 *
 * Borrowed shared-memory frames are copied: the producer reuses its buffer as
 * soon as we give it back, and holding it instead would throttle capture to
 * the encoder's pace.
 * Copying is deleted to prevent double-close; use move semantics instead.
 */
struct QueuedFrame : public VideoFrame {
  std::vector<uint8_t> ownedData;

  explicit QueuedFrame(const VideoFrame &other) : VideoFrame(other) {
    if (!other.isDmaBuf && other.data && other.stride > 0 && other.height > 0) {
      // Share an already-owned snapshot; never lease a producer SHM buffer.
      if (other.cpuStorage && other.data == other.cpuStorage->data() &&
          other.cpuStorage->size() >= size_t(other.stride) * size_t(other.height)) {
        return;
      }
      cpuStorage.reset();
      // Copy borrowed shared-memory frames. Leasing them instead made capture
      // wait for the encoder (the producer's pool is the only decoupling we
      // have) and produced visibly uneven delivery with software encoders.
      ownedData.assign(other.data,
                       other.data + (size_t(other.stride) * other.height));
      data = ownedData.data();
    } else if (other.isDmaBuf) {
      if (!retainDmaBuf()) {
        isDmaBuf = false;
        dmaBufFd = -1;
        data = nullptr;
      }
    }
  }

  // Move constructor – transfer fd ownership
  QueuedFrame(QueuedFrame &&other) noexcept
      : VideoFrame(static_cast<VideoFrame &&>(other)),
        ownedData(std::move(other.ownedData)) {
    if (!ownedData.empty())
      data = ownedData.data();
    other.dmaBufFd = -1;
    other.dmaBufPlanes = {};
  }

  // Move assignment – transfer fd ownership
  QueuedFrame &operator=(QueuedFrame &&other) noexcept {
    if (this != &other) {
      static_cast<VideoFrame &>(*this) = static_cast<VideoFrame &&>(other);
      ownedData = std::move(other.ownedData);
      if (!ownedData.empty())
        data = ownedData.data();
      other.dmaBufFd = -1;
      other.dmaBufPlanes = {};
    }
    return *this;
  }

  QueuedFrame(const QueuedFrame &) = delete;
  QueuedFrame &operator=(const QueuedFrame &) = delete;

  ~QueuedFrame() = default;
};

/**
 * @brief Configuration for video capture
 */
struct CaptureConfig {
  int targetFps = 60;
  int maxWidth = 0;  // 0 = native resolution
  int maxHeight = 0; // 0 = native resolution
  bool captureMouseCursor = true;
  bool useDmaBuf = true;
  QString waylandRestoreToken;
  bool persistPermission = true; // Diagnostics request an ephemeral grant.
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
  // SourceUnavailable includes visibility/permission loss and is not retryable.
  // SourceDestroyed requires an independent same-daemon/original-node check.
  // A portal Session.Closed signal alone cannot establish window destruction.
  // SourceNotReady is a successful portal grant with no stream to capture yet;
  // only an already-authorized automatic game capture may retry that condition.
  enum class CaptureErrorKind {
    Other, SourceUnavailable, SourceDestroyed, SourceNotReady, SessionClosed,
    Cancelled
  };
  virtual CaptureErrorKind lastErrorKind() const { return CaptureErrorKind::Other; }
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
