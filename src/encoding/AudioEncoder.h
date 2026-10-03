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

extern "C" {
#include <libavutil/samplefmt.h>
}

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

  /**
   * @brief Shift all output timestamps forward by this many samples.
   *
   * For tracks whose first audio arrives after the first video frame:
   * counting from zero would bake the startup skew in for the whole
   * file. Offsetting once at first feed aligns the track to video.
   * Call once before/at first feed; later calls are ignored.
   */
  void setStartOffsetSamples(int64_t offset) {
    if (!m_offsetApplied) {
      m_startOffset = offset > 0 ? offset : 0;
      m_offsetApplied = true;
    }
  }

signals:
  void encodingError(const QString &error);

private:
  void cleanup();
  bool processPacket(AVPacket *packet, EncodedPacket &output);
  // Convert one frame from the interleaved-float accumulator into m_frame,
  // sanitizing non-finite PipeWire samples to silence.
  void writeFrameSamples();

  AVCodecContext *m_codecCtx = nullptr;
  AVFrame *m_frame = nullptr;
  AVPacket *m_packet = nullptr;
  AVSampleFormat m_sampleFmt = AV_SAMPLE_FMT_FLTP;

  int64_t m_sampleCount = 0;
  // One-time timestamp origin shift (see setStartOffsetSamples).
  int64_t m_startOffset = 0;
  bool m_offsetApplied = false;
  bool m_initialized = false;

  // Frame accumulation buffer (AAC requires exactly 1024 samples per frame)
  std::vector<float> m_accumBuffer;
  int m_requiredFrameSize = 1024;  // AAC standard frame size
};

} // namespace TrophyBackend
