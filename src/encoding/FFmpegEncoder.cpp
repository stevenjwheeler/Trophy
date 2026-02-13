/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FFmpegEncoder.h"
#include "capture/IVideoCaptureSource.h"

#include <QDebug>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace GameRecorder {

FFmpegEncoder::FFmpegEncoder(EncoderType type, const QString &codecName,
                             QObject *parent)
    : IEncoder(parent), m_type(type), m_codecName(codecName) {
  // Set display name
  switch (type) {
  case EncoderType::NVENC_H264:
    m_name = "NVENC H.264";
    break;
  case EncoderType::NVENC_HEVC:
    m_name = "NVENC HEVC";
    break;
  case EncoderType::VAAPI_H264:
    m_name = "VA-API H.264";
    break;
  case EncoderType::VAAPI_HEVC:
    m_name = "VA-API HEVC";
    break;
  case EncoderType::Software_x264:
    m_name = "x264";
    break;
  case EncoderType::Software_x265:
    m_name = "x265";
    break;
  }
}

FFmpegEncoder::~FFmpegEncoder() { cleanup(); }

bool FFmpegEncoder::initialize(const EncoderConfig &config) {
  if (m_initialized) {
    cleanup();
  }

  qInfo() << "Initializing encoder:" << m_name;
  qInfo() << "  Resolution:" << config.width << "x" << config.height;
  qInfo() << "  FPS:" << config.fps;
  qInfo() << "  Bitrate:" << config.bitrate << "kbps";

  if (!initializeCodec(config)) {
    return false;
  }

  // Allocate frame
  m_frame = av_frame_alloc();
  if (!m_frame) {
    qCritical() << "Failed to allocate frame";
    cleanup();
    return false;
  }

  m_frame->format = m_codecCtx->pix_fmt;
  m_frame->width = config.width;
  m_frame->height = config.height;

  if (av_frame_get_buffer(m_frame, 0) < 0) {
    qCritical() << "Failed to allocate frame buffer";
    cleanup();
    return false;
  }

  // Allocate packet
  m_packet = av_packet_alloc();
  if (!m_packet) {
    qCritical() << "Failed to allocate packet";
    cleanup();
    return false;
  }

  m_frameCount = 0;

  // Initialize scaling context: Convert from BGRA (Screen) to Encoder's format
  m_swsCtx = sws_getContext(
      config.width, config.height, AV_PIX_FMT_BGRA,     // Source
      config.width, config.height, m_codecCtx->pix_fmt, // Destination
      SWS_BICUBIC, nullptr, nullptr, nullptr);

  if (!m_swsCtx) {
    qCritical() << "Failed to create scaling context";
    cleanup();
    return false;
  }

  m_initialized = true;

  qInfo() << "Encoder initialized successfully";
  return true;
}

bool FFmpegEncoder::initializeCodec(const EncoderConfig &config) {
  const AVCodec *codec =
      avcodec_find_encoder_by_name(m_codecName.toUtf8().constData());
  if (!codec) {
    qCritical() << "Codec not found:" << m_codecName;
    return false;
  }

  m_codecCtx = avcodec_alloc_context3(codec);
  if (!m_codecCtx) {
    qCritical() << "Failed to allocate codec context";
    return false;
  }

  // Set encoding parameters
  m_codecCtx->width = config.width;
  m_codecCtx->height = config.height;
  m_codecCtx->time_base = {1, config.fps};
  m_codecCtx->framerate = {config.fps, 1};
  m_codecCtx->gop_size = config.fps * config.keyframeInterval;
  m_codecCtx->max_b_frames = 2;

  // Bitrate settings
  if (config.useVBR) {
    m_codecCtx->bit_rate = config.bitrate * 1000;
    m_codecCtx->rc_max_rate = config.maxBitrate * 1000;
    m_codecCtx->rc_buffer_size = config.maxBitrate * 1000 * 2;
  } else {
    m_codecCtx->bit_rate = config.bitrate * 1000;
  }

  // Use microseconds for internal timebase
  m_codecCtx->time_base = {1, 1000000};
  m_codecCtx->framerate = {config.fps, 1};
  m_firstTimestamp = -1;

  // Pixel format
  if (isHardwareEncoder()) {
    if (m_type == EncoderType::NVENC_H264 ||
        m_type == EncoderType::NVENC_HEVC) {
      m_codecCtx->pix_fmt = AV_PIX_FMT_NV12;
    } else if (m_type == EncoderType::VAAPI_H264 ||
               m_type == EncoderType::VAAPI_HEVC) {
      m_codecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
      if (!initializeHardwareContext()) {
        return false;
      }
    }
  } else {
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  }

  // Encoder-specific options
  AVDictionary *opts = nullptr;

  if (m_type == EncoderType::NVENC_H264 || m_type == EncoderType::NVENC_HEVC) {
    av_dict_set(&opts, "preset", config.preset.toUtf8().constData(), 0);
    av_dict_set(&opts, "rc", "vbr", 0);
    av_dict_set(&opts, "cq", "23", 0);
  } else if (m_type == EncoderType::Software_x264) {
    av_dict_set(&opts, "preset", config.preset.toUtf8().constData(), 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "profile", config.profile.toUtf8().constData(), 0);
  }

  // Modern containers (MP4, MKV) often need global headers
  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int ret = avcodec_open2(m_codecCtx, codec, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to open codec:" << err;
    return false;
  }

  return true;
}

bool FFmpegEncoder::initializeHardwareContext() {
#ifdef HAVE_VAAPI
  int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI,
                                   "/dev/dri/renderD128", nullptr, 0);
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to create VA-API device context:" << err;
    return false;
  }

  m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

  // Create frames context
  m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
  if (!m_hwFramesCtx) {
    qCritical() << "Failed to allocate HW frames context";
    return false;
  }

  AVHWFramesContext *framesCtx =
      reinterpret_cast<AVHWFramesContext *>(m_hwFramesCtx->data);
  framesCtx->format = AV_PIX_FMT_VAAPI;
  framesCtx->sw_format = AV_PIX_FMT_NV12;
  framesCtx->width = m_codecCtx->width;
  framesCtx->height = m_codecCtx->height;
  framesCtx->initial_pool_size = 20;

  ret = av_hwframe_ctx_init(m_hwFramesCtx);
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to initialize HW frames context:" << err;
    return false;
  }

  m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);

  qInfo() << "VA-API hardware context initialized";
  return true;
#else
  return false;
#endif
}

bool FFmpegEncoder::encodeFrame(const VideoFrame &frame,
                                EncodedPacket &output) {
  if (!m_initialized) {
    emit encodingError("Encoder not initialized");
    return false;
  }

  // Make frame writable
  if (av_frame_make_writable(m_frame) < 0) {
    emit encodingError("Failed to make frame writable");
    return false;
  }

  // Convert colors from BGRA (Screen) to Encoder Format
  const uint8_t *srcData[1] = {frame.data};
  int srcLinesize[1] = {(int)frame.stride};

  sws_scale(m_swsCtx, srcData, srcLinesize, 0, frame.height, m_frame->data,
            m_frame->linesize);

  // If using hardware encoding (VAAPI/NVENC), we might need to transfer to GPU
  if (m_codecCtx->hw_frames_ctx) {
    if (!m_hwFrame) {
      m_hwFrame = av_frame_alloc();
      av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, m_hwFrame, 0);
    }
    av_hwframe_transfer_data(m_hwFrame, m_frame, 0);
  }

  // Set presentation timestamp
  if (m_firstTimestamp < 0) {
    m_firstTimestamp = frame.timestamp;
  }
  m_frame->pts = frame.timestamp - m_firstTimestamp;
  m_frameCount++;

  // Send frame to encoder
  int ret = avcodec_send_frame(m_codecCtx, m_frame);
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    emit encodingError(QString("Error sending frame: %1").arg(err));
    return false;
  }

  // Receive encoded packet
  ret = avcodec_receive_packet(m_codecCtx, m_packet);
  if (ret == AVERROR(EAGAIN)) {
    // Need more frames
    return false;
  }
  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    emit encodingError(QString("Error receiving packet: %1").arg(err));
    return false;
  }

  return processPacket(m_packet, output);
}

int FFmpegEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized) {
    return 0;
  }

  // Send null frame to flush
  avcodec_send_frame(m_codecCtx, nullptr);

  int count = 0;
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

bool FFmpegEncoder::processPacket(AVPacket *packet, EncodedPacket &output) {
  output.data.resize(packet->size);
  memcpy(output.data.data(), packet->data, packet->size);
  output.pts = packet->pts;
  output.dts = packet->dts;
  output.isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  output.streamIndex = 0;

  av_packet_unref(packet);
  return true;
}

bool FFmpegEncoder::isHardwareEncoder() const {
  return m_type == EncoderType::NVENC_H264 ||
         m_type == EncoderType::NVENC_HEVC ||
         m_type == EncoderType::VAAPI_H264 || m_type == EncoderType::VAAPI_HEVC;
}

std::vector<uint8_t> FFmpegEncoder::extraData() const {
  if (!m_codecCtx || !m_codecCtx->extradata) {
    return {};
  }
  return std::vector<uint8_t>(m_codecCtx->extradata,
                              m_codecCtx->extradata +
                                  m_codecCtx->extradata_size);
}

int FFmpegEncoder::codecId() const {
  return m_codecCtx ? m_codecCtx->codec_id : 0;
}

void FFmpegEncoder::cleanup() {
  if (m_swsCtx) {
    sws_freeContext(m_swsCtx);
    m_swsCtx = nullptr;
  }
  if (m_packet) {
    av_packet_free(&m_packet);
  }
  if (m_hwFrame) {
    av_frame_free(&m_hwFrame);
  }
  if (m_frame) {
    av_frame_free(&m_frame);
  }
  if (m_hwFramesCtx) {
    av_buffer_unref(&m_hwFramesCtx);
  }
  if (m_hwDeviceCtx) {
    av_buffer_unref(&m_hwDeviceCtx);
  }
  if (m_codecCtx) {
    avcodec_free_context(&m_codecCtx);
  }

  m_initialized = false;
}

} // namespace GameRecorder
