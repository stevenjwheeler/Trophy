/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IEncoder.h"
#include <QString>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace TrophyBackend {

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

private:
  bool initializeCodec(const EncoderConfig &config);
  bool initializeHardwareContext();
  void cleanup();
  bool processPacket(AVPacket *packet, EncodedPacket &output);

  int m_lastInputWidth = 0;
  int m_lastInputHeight = 0;
  int m_swsInputFmt = 0; // AVPixelFormat of the last sws context

  EncoderType m_type;
  QString m_codecName;
  QString m_name;
  EncoderConfig m_config;

  AVCodecContext *m_codecCtx = nullptr;
  AVFrame *m_frame = nullptr;
  AVFrame *m_hwFrame = nullptr; // For hardware encoding
  AVPacket *m_packet = nullptr;
  SwsContext *m_swsCtx = nullptr;
  AVBufferRef *m_hwDeviceCtx = nullptr;  // VA-API / CUDA device
  AVBufferRef *m_drmDeviceCtx = nullptr; // DRM device (parent of VA-API ctx)
  AVBufferRef *m_hwFramesCtx = nullptr;

  int64_t m_frameCount = 0;
  int64_t m_firstTimestamp = -1;
  bool m_initialized = false;
};

} // namespace TrophyBackend
