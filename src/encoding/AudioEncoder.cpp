/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioEncoder.h"
#include "IEncoder.h"

#include <QDebug>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace TrophyBackend {

AudioEncoder::AudioEncoder(QObject *parent) : QObject(parent) {}

AudioEncoder::~AudioEncoder() { cleanup(); }

bool AudioEncoder::initialize(const AudioEncoderConfig &config) {
  if (m_initialized) {
    cleanup();
  }

  // Find encoder
  const AVCodec *codec = nullptr;
  if (config.codec == "opus") {
    codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
  } else {
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  }

  if (!codec) {
    qCritical() << "Audio codec not found:" << config.codec;
    return false;
  }

  m_codecCtx = avcodec_alloc_context3(codec);
  if (!m_codecCtx) {
    qCritical() << "Failed to allocate audio codec context";
    return false;
  }

  // Configure encoder
  m_codecCtx->sample_rate = config.sampleRate;
  m_codecCtx->bit_rate = config.bitrate * 1000;
  m_codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
  m_codecCtx->time_base = {1, 1000};

  // Set channel layout
  av_channel_layout_default(&m_codecCtx->ch_layout, config.channels);

  // Ensure extradata (AudioSpecificConfig) is written to codecpar, not packets
  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  // Open encoder
  int ret = avcodec_open2(m_codecCtx, codec, nullptr);
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to open audio codec:" << err;
    cleanup();
    return false;
  }

  // Allocate frame
  m_frame = av_frame_alloc();
  if (!m_frame) {
    cleanup();
    return false;
  }

  m_frame->format = m_codecCtx->sample_fmt;
  m_frame->nb_samples = m_codecCtx->frame_size;
  av_channel_layout_copy(&m_frame->ch_layout, &m_codecCtx->ch_layout);

  if (av_frame_get_buffer(m_frame, 0) < 0) {
    qCritical() << "Failed to allocate audio frame buffer";
    cleanup();
    return false;
  }

  // Allocate packet
  m_packet = av_packet_alloc();
  if (!m_packet) {
    cleanup();
    return false;
  }

  m_sampleCount = 0;
  m_requiredFrameSize = m_codecCtx->frame_size > 0 ? m_codecCtx->frame_size : 1024;
  m_initialized = true;

  qDebug() << "Audio encoder initialized:" << config.codec << config.sampleRate
           << "Hz," << config.channels << "ch," << config.bitrate << "kbps"
           << "Frame size:" << m_requiredFrameSize;

  return true;
}

bool AudioEncoder::encodeAudio(const float *samples, int frameCount,
                               EncodedPacket &output) {
  if (!m_initialized) {
    return false;
  }

  int channels = m_frame->ch_layout.nb_channels;
  int totalIncomingSamples = frameCount * channels;

  // Accumulate samples into buffer
  m_accumBuffer.insert(m_accumBuffer.end(), samples,
                       samples + totalIncomingSamples);

  // Process complete frames from the accumulator
  while (static_cast<int>(m_accumBuffer.size()) >=
         m_requiredFrameSize * channels) {
    if (av_frame_make_writable(m_frame) < 0) {
      emit encodingError("Failed to make audio frame writable");
      return false;
    }

    // Convert interleaved to planar and sanitize NaN/Inf from PipeWire
    for (int ch = 0; ch < channels; ch++) {
      float *dst = reinterpret_cast<float *>(m_frame->data[ch]);
      for (int s = 0; s < m_requiredFrameSize; s++) {
        float v = m_accumBuffer[s * channels + ch];
        dst[s] = std::isfinite(v) ? v : 0.0f;
      }
    }

    // Set frame properties
    m_frame->nb_samples = m_requiredFrameSize;
    m_frame->pts = (m_sampleCount * 1000LL) / m_codecCtx->sample_rate;
    m_sampleCount += m_requiredFrameSize;

    // Send frame to encoder
    int ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) {
      char err[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, err, sizeof(err));
      emit encodingError(QString("Error sending audio frame: %1").arg(err));
      return false;
    }

    // Try to receive a packet
    ret = avcodec_receive_packet(m_codecCtx, m_packet);
    if (ret == AVERROR(EAGAIN)) {
      // Encoder needs more frames, remove processed samples and continue
      m_accumBuffer.erase(m_accumBuffer.begin(),
                          m_accumBuffer.begin() +
                              m_requiredFrameSize * channels);
      continue;
    }
    if (ret < 0) {
      // Error or EOF
      m_accumBuffer.erase(m_accumBuffer.begin(),
                          m_accumBuffer.begin() +
                              m_requiredFrameSize * channels);
      return false;
    }

    // Got a packet! Remove processed samples and return
    m_accumBuffer.erase(m_accumBuffer.begin(),
                        m_accumBuffer.begin() + m_requiredFrameSize * channels);
    return processPacket(m_packet, output);
  }

  // Buffer has fewer than required samples, wait for more
  return false;
}

int AudioEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized) {
    return 0;
  }

  int count = 0;
  int channels = m_frame->ch_layout.nb_channels;

  // Flush any remaining buffered samples by padding with zeros
  if (!m_accumBuffer.empty()) {
    int remaining = m_accumBuffer.size() / channels;
    if (remaining > 0 && remaining < m_requiredFrameSize) {
      // Pad with zeros to complete the frame
      m_accumBuffer.resize(m_requiredFrameSize * channels, 0.0f);

      if (av_frame_make_writable(m_frame) == 0) {
        // Convert and send the padded frame
        for (int ch = 0; ch < channels; ch++) {
          float *dst = reinterpret_cast<float *>(m_frame->data[ch]);
          for (int s = 0; s < m_requiredFrameSize; s++) {
            float v = m_accumBuffer[s * channels + ch];
            dst[s] = std::isfinite(v) ? v : 0.0f;
          }
        }

        m_frame->nb_samples = m_requiredFrameSize;
        m_frame->pts = (m_sampleCount * 1000LL) / m_codecCtx->sample_rate;
        m_sampleCount += m_requiredFrameSize;

        avcodec_send_frame(m_codecCtx, m_frame);
      }
      m_accumBuffer.clear();
    }
  }

  // Send EOF to encoder
  avcodec_send_frame(m_codecCtx, nullptr);

  // Receive all remaining packets
  while (true) {
    int ret = avcodec_receive_packet(m_codecCtx, m_packet);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      break;
    }
    if (ret < 0) {
      break;
    }

    EncodedPacket output;
    if (processPacket(m_packet, output)) {
      packets.push_back(std::move(output));
      count++;
    }
  }

  return count;
}

bool AudioEncoder::processPacket(AVPacket *packet, EncodedPacket &output) {
  output.data.resize(packet->size);
  memcpy(output.data.data(), packet->data, packet->size);
  output.pts = packet->pts;
  output.dts = packet->dts;
  output.duration = packet->duration;
  output.isKeyframe = true; // Audio frames are always keyframes
  output.streamIndex = 1;   // Audio stream

  av_packet_unref(packet);
  return true;
}

int AudioEncoder::codecId() const {
  return m_codecCtx ? m_codecCtx->codec_id : 0;
}

std::vector<uint8_t> AudioEncoder::extraData() const {
  if (!m_codecCtx || !m_codecCtx->extradata) {
    return {};
  }
  return std::vector<uint8_t>(m_codecCtx->extradata,
                              m_codecCtx->extradata +
                                  m_codecCtx->extradata_size);
}

void AudioEncoder::cleanup() {
  if (m_packet) {
    av_packet_free(&m_packet);
  }
  if (m_frame) {
    av_frame_free(&m_frame);
  }
  if (m_codecCtx) {
    avcodec_free_context(&m_codecCtx);
  }
  m_initialized = false;
}

} // namespace TrophyBackend
