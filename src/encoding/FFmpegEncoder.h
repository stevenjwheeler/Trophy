/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IEncoder.h"
#include <QString>
#include <atomic>
#include <cstdint>
#include <memory>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace TrophyBackend {

// Stage B import state (defined in FFmpegEncoder.cpp; needs no CUDA
// toolkit headers here).
struct CudaImportState;
struct VaapiImportState;

/**
 * @brief FFmpeg-based video encoder implementation
 *
 * Supports NVENC, VA-API, and software (x264) encoding.
 */
class FFmpegEncoder : public IEncoder {
  Q_OBJECT

public:
  bool isInitialized() const { return m_initialized; }

  FFmpegEncoder(EncoderType type, const QString &codecName,
                QObject *parent = nullptr);
  ~FFmpegEncoder() override;

  bool initialize(const EncoderConfig &config) override;
  EncoderConfig config() const override { return m_config; }
  bool encodeFrame(const VideoFrame &frame, EncodedPacket &output) override;
  int flush(std::vector<EncodedPacket> &packets) override;

  EncoderType type() const override { return m_type; }
  QString name() const override { return m_name; }
  bool isHardwareEncoder() const override;
  std::vector<uint8_t> extraData() const override;
  int codecId() const override;
  int encodedPixelFormat() const override;

  // Zero-copy telemetry: input frames encoded via the DMA-BUF hwmap path
  // vs. the CPU fallback, and inputs that produced no frame at all.
  int64_t zeroCopyFrames() const { return m_zeroCopyFrames.load(); }
  int64_t eglImportFrames() const { return m_eglImportFrames.load(); }
  int64_t gpuScaledFrames() const { return m_gpuScaledFrames.load(); }
  int64_t vaapiVppFrames() const { return m_vaapiVppFrames.load(); }
  int64_t fallbackFrames() const { return m_fallbackFrames.load(); }
  int64_t droppedFrames() const { return m_droppedFrames.load(); }

private:
  friend class FFmpegEncoderTestAccess;
  bool initializeCodec(const EncoderConfig &config);
  bool initializeHardwareContext();
  void cleanup();
  bool processPacket(AVPacket *packet, EncodedPacket &output);
  void logSummary();
  bool convertSoftwareFrame(const VideoFrame &frame);

  int m_lastInputWidth = 0;
  int m_lastInputHeight = 0;
  int m_swsInputFmt = 0; // AVPixelFormat of the last sws context

  EncoderType m_type;
  QString m_codecName;
  QString m_name;
  EncoderConfig m_config;

  AVCodecContext *m_codecCtx = nullptr;
  AVFrame *m_frame = nullptr;
  AVPacket *m_packet = nullptr;
  SwsContext *m_swsCtx = nullptr;
  AVBufferRef *m_hwDeviceCtx = nullptr;  // VA-API / CUDA device
  AVBufferRef *m_hwFramesCtx = nullptr;

  int64_t m_frameCount = 0;
  int64_t m_firstTimestamp = -1;
  bool m_initialized = false;

  // Step-1 observability: zero-copy vs. fallback accounting. Written
  // on the encode thread, read via GUI getters: atomics, no lock needed.
  // (FFmpegEncoder.cpp uses only m_x = 0 / m_x++ / reads / sums, all of
  // which work on atomics via store/fetch-add/load-conversion.)
  std::atomic<int64_t> m_zeroCopyFrames{0};
  std::atomic<int64_t> m_eglImportFrames{0};
  std::atomic<int64_t> m_gpuScaledFrames{0};
  std::atomic<int64_t> m_vaapiVppFrames{0};
  std::atomic<int64_t> m_fallbackFrames{0};
  std::atomic<int64_t> m_droppedFrames{0};
  bool m_lastMapSucceeded = true; // suppress transition spam; first failure logs
  bool m_lastImportSucceeded = true;
  bool m_summaryLogged = false;
  std::unique_ptr<CudaImportState> m_cudaImport;
  std::unique_ptr<VaapiImportState> m_vaapiImport;
};

} // namespace TrophyBackend
