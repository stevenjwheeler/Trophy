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

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace TrophyBackend {

struct EncodedPacket;

/**
 * @brief Audio encoder configuration
 */
struct AudioEncoderConfig {
  int sampleRate = 48000;
  int channels = 2;
  int bitrate = 192;     // kbps
  QString codec = "aac"; // aac, opus
};

/**
 * @brief FFmpeg-based audio encoder
 *
 * Encodes audio samples to AAC or Opus format.
 */
class AudioEncoder : public QObject {
  Q_OBJECT

public:
  explicit AudioEncoder(QObject *parent = nullptr);
  ~AudioEncoder() override;

  bool initialize(const AudioEncoderConfig &config);

  /**
   * @brief Encode audio samples
   * @param samples Interleaved float samples
   * @param frameCount Number of frames
   * @param output Encoded packet output
   * @return true if packet was produced
   */
  bool encodeAudio(const float *samples, int frameCount, EncodedPacket &output);

  /**
   * @brief Flush encoder
   * @param packets Output packets
   * @return Number of packets flushed
   */
  int flush(std::vector<EncodedPacket> &packets);

  int codecId() const;
  std::vector<uint8_t> extraData() const;

signals:
  void encodingError(const QString &error);

private:
  void cleanup();
  bool processPacket(AVPacket *packet, EncodedPacket &output);

  AVCodecContext *m_codecCtx = nullptr;
  AVFrame *m_frame = nullptr;
  AVPacket *m_packet = nullptr;

  int64_t m_sampleCount = 0;
  bool m_initialized = false;

  // Frame accumulation buffer (AAC requires exactly 1024 samples per frame)
  std::vector<float> m_accumBuffer;
  int m_requiredFrameSize = 1024;  // AAC standard frame size
};

} // namespace TrophyBackend
