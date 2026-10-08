/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <vector>

namespace TrophyBackend {

struct VideoFrame; // Forward declaration

/**
 * @brief Encoded packet output from encoder
 */
struct EncodedPacket {
  std::vector<uint8_t> data;
  int64_t pts = 0; // Presentation timestamp
  int64_t dts = 0; // Decode timestamp
  int64_t duration = 0; // Frame duration
  bool isKeyframe = false;
  int streamIndex = 0; // For muxer
};

/**
 * @brief Encoder configuration
 */
struct EncoderConfig {
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int bitrate = 15000;      // kbps
  int maxBitrate = 20000;   // kbps, for VBR
  QString preset = "fast";  // Encoder-specific preset
  QString profile = "high"; // H.264 profile
  int keyframeInterval = 2; // Seconds between keyframes
  bool useVBR = true;
  int inputPixelFormat = 0; // AVPixelFormat of input frames
};

/**
 * @brief Abstract interface for video encoders
 *
 * Implementations include NVENC, VA-API, and software (x264/AV1) encoders.
 */
class IEncoder : public QObject {
  Q_OBJECT

public:
  enum class EncoderType {
    NVENC_H264,
    NVENC_HEVC,
    NVENC_AV1,
    VAAPI_H264,
    VAAPI_HEVC,
    VAAPI_AV1,
    Software_x264
  };
  Q_ENUM(EncoderType)

  explicit IEncoder(QObject *parent = nullptr) : QObject(parent) {}
  ~IEncoder() override = default;

  /**
   * @brief Get the configuration this encoder was initialized with
   */
  virtual EncoderConfig config() const = 0;

  // Helpers that pull from the virtual config()
  int width() const { return config().width; }
  int height() const { return config().height; }
  int fps() const { return config().fps; }
  int bitrate() const { return config().bitrate; }
  QString preset() const { return config().preset; }

  /**
   * @brief Initialize encoder with configuration
   * @param config Encoder settings
   * @return true on success
   */
  virtual bool initialize(const EncoderConfig &config) = 0;

  /**
   * @brief Encode a video frame
   * @param frame Input frame to encode
   * @param output Encoded packet output (only valid when true is returned)
   * @return true if a packet was produced and `output` is safe to mux;
   *         false if the frame was buffered (EAGAIN), dropped, or errored.
   */
  virtual bool encodeFrame(const VideoFrame &frame, EncodedPacket &output) = 0;

  /**
   * @brief Flush encoder and get remaining packets
   * @param packets Output vector for remaining packets
   * @return Number of packets flushed
   */
  virtual int flush(std::vector<EncodedPacket> &packets) = 0;

  /**
   * @brief Get encoder type
   */
  virtual EncoderType type() const = 0;

  /**
   * @brief Get human-readable encoder name
   */
  virtual QString name() const = 0;

  /**
   * @brief Check if this is a hardware encoder
   */
  virtual bool isHardwareEncoder() const = 0;

  /**
   * @brief Get codec extradata (SPS/PPS for H.264)
   */
  virtual std::vector<uint8_t> extraData() const = 0;

  /**
   * @brief Get the codec's FFmpeg codec ID
   */
  virtual int codecId() const = 0;

  /**
   * @brief Get the encoder's output pixel format for container metadata
   */
  virtual int encodedPixelFormat() const = 0;

signals:
  void encodingError(const QString &error);
};

} // namespace TrophyBackend
