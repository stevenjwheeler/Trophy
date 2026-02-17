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

  m_config = config;

  qInfo() << "Initializing encoder:" << m_name;
  qInfo() << "  Target Resolution:" << config.width << "x" << config.height;
  qInfo() << "  FPS:" << config.fps;
  qInfo() << "  Bitrate:" << config.bitrate << "kbps";

  m_lastInputWidth = 0;
  m_lastInputHeight = 0;

  if (!initializeCodec(config)) {
    return false;
  }

  // Allocate software frame at TARGET resolution
  m_frame = av_frame_alloc();
  if (!m_frame) {
    qCritical() << "Failed to allocate frame";
    cleanup();
    return false;
  }

  m_frame->format =
      (isHardwareEncoder()) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  m_frame->width = config.width;
  m_frame->height = config.height;

  int ret =
      av_frame_get_buffer(m_frame, 0); // 0 means align for the CPU/Encoder
  if (ret < 0) {
    qCritical() << "Could not allocate the video frame data";
    return false;
  }

  m_packet = av_packet_alloc();
  if (!m_packet) {
    qCritical() << "Failed to allocate packet";
    cleanup();
    return false;
  }

  m_frameCount = 0;
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
  if (!m_codecCtx)
    return false;

  // 1. Basic Configuration
  m_codecCtx->width = config.width;
  m_codecCtx->height = config.height;
  m_codecCtx->time_base = {1, 1000000};
  m_codecCtx->framerate = {config.fps, 1};
  m_codecCtx->gop_size = config.fps * 2;
  m_codecCtx->max_b_frames = 2;

  // 2. Bitrate Configuration
  int64_t bps = static_cast<int64_t>(config.bitrate) * 1000;
  m_codecCtx->bit_rate = bps;
  if (config.useVBR) {
    m_codecCtx->rc_max_rate = static_cast<int64_t>(config.maxBitrate) * 1000;
    m_codecCtx->rc_buffer_size = m_codecCtx->rc_max_rate * 2;
  } else {
    m_codecCtx->rc_max_rate = bps;
    m_codecCtx->rc_buffer_size = bps * 2;
  }

  // 3. Pixel Format & Hardware Context
  if (isHardwareEncoder()) {
    if (m_type == EncoderType::NVENC_H264 ||
        m_type == EncoderType::NVENC_HEVC) {
      m_codecCtx->pix_fmt = AV_PIX_FMT_NV12;
    } else if (m_type == EncoderType::VAAPI_H264 ||
               m_type == EncoderType::VAAPI_HEVC) {
      m_codecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
      if (!initializeHardwareContext())
        return false;
    }
  } else {
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  }

  // 4. Preset Translation and Options
  AVDictionary *opts = nullptr;
  QString preset = config.preset.toLower();

  if (m_type == EncoderType::NVENC_H264 || m_type == EncoderType::NVENC_HEVC) {
    QString nvPreset = "p4";
    if (preset == "ultrafast" || preset == "superfast")
      nvPreset = "p1";
    else if (preset == "veryfast" || preset == "faster")
      nvPreset = "p2";
    else if (preset == "fast")
      nvPreset = "p3";
    else if (preset == "medium")
      nvPreset = "p4";
    else if (preset == "slow")
      nvPreset = "p5";
    else if (preset == "slower")
      nvPreset = "p6";
    else if (preset == "veryslow")
      nvPreset = "p7";

    av_dict_set(&opts, "preset", nvPreset.toUtf8().constData(), 0);
    av_dict_set(&opts, "rc", config.useVBR ? "vbr" : "cbr", 0);
    av_dict_set(&opts, "rc-lookahead", "20", 0);

  } else if (m_type == EncoderType::VAAPI_H264 ||
             m_type == EncoderType::VAAPI_HEVC) {
    QString vaLevel = "4";
    if (preset == "ultrafast" || preset == "superfast")
      vaLevel = "7";
    else if (preset == "veryfast")
      vaLevel = "6";
    else if (preset == "faster")
      vaLevel = "5";
    else if (preset == "fast")
      vaLevel = "4";
    else if (preset == "medium")
      vaLevel = "3";
    else if (preset == "slow")
      vaLevel = "2";
    else if (preset == "slower" || preset == "veryslow")
      vaLevel = "1";

    av_dict_set(&opts, "compression_level", vaLevel.toUtf8().constData(), 0);

  } else {
    av_dict_set(&opts, "preset", preset.toUtf8().constData(), 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
  }

  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  // 5. Open Codec
  int ret = avcodec_open2(m_codecCtx, codec, &opts);
  av_dict_free(&opts); // Clean up immediately after opening

  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to open codec:" << err;
    return false;
  }

  m_firstTimestamp = -1;
  return true;
}

bool FFmpegEncoder::initializeHardwareContext() {
#ifdef HAVE_VAAPI
  // nullptr lets FFmpeg enumerate /dev/dri/renderD* and pick the right device
  int ret = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI,
                                   nullptr, nullptr, 0);
  if (ret < 0)
    return false;

  m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
  m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
  if (!m_hwFramesCtx)
    return false;

  AVHWFramesContext *framesCtx =
      reinterpret_cast<AVHWFramesContext *>(m_hwFramesCtx->data);
  framesCtx->format = AV_PIX_FMT_VAAPI;
  framesCtx->sw_format = AV_PIX_FMT_NV12;
  framesCtx->width = m_codecCtx->width;
  framesCtx->height = m_codecCtx->height;
  framesCtx->initial_pool_size = 20;

  ret = av_hwframe_ctx_init(m_hwFramesCtx);
  if (ret < 0)
    return false;

  m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
  return true;
#else
  return false;
#endif
}

bool FFmpegEncoder::encodeFrame(const VideoFrame &frame,
                                EncodedPacket &output) {
  if (!m_initialized)
    return false;

  // 1. Re-init scaler if source window size changed
  if (!m_swsCtx || m_lastInputWidth != frame.width ||
      m_lastInputHeight != frame.height) {
    if (m_swsCtx)
      sws_freeContext(m_swsCtx);

    // Determine the target format for the scaler based on encoder type
    AVPixelFormat targetFmt =
        (m_codecCtx->hw_device_ctx || m_type == EncoderType::NVENC_H264 ||
         m_type == EncoderType::NVENC_HEVC)
            ? AV_PIX_FMT_NV12
            : AV_PIX_FMT_YUV420P;

    m_swsCtx = sws_getContext(frame.width, frame.height, AV_PIX_FMT_BGRA,
                              m_config.width, m_config.height, targetFmt,
                              SWS_BICUBIC, nullptr, nullptr, nullptr);

    m_lastInputWidth = frame.width;
    m_lastInputHeight = frame.height;

    if (!m_swsCtx)
      return false;
  }

  // 2. Ensure frame is ready to be written to
  if (av_frame_make_writable(m_frame) < 0)
    return false;

  // 3. Setup pointers for the Scaler
  const uint8_t *srcData[4] = {frame.data, nullptr, nullptr, nullptr};
  int srcLinesize[4] = {static_cast<int>(frame.stride), 0, 0, 0};

  // 4. Perform scaling
  // We use m_frame->data and m_frame->linesize directly.
  // If these are "bad", swscaler logs the error you see.
  int sliceHeight = sws_scale(m_swsCtx, srcData, srcLinesize, 0, frame.height,
                              m_frame->data, m_frame->linesize);

  if (sliceHeight <= 0) {
    qWarning() << "Scaling failed!";
    return false;
  }

  // 5. Handle Timestamps
  if (m_firstTimestamp < 0)
    m_firstTimestamp = frame.timestamp;
  m_frame->pts = frame.timestamp - m_firstTimestamp;

  AVFrame *inputFrame = m_frame;

  // 6. Hardware Transfer (GPU)
  if (m_codecCtx->hw_frames_ctx) {
    if (!m_hwFrame) {
      m_hwFrame = av_frame_alloc();
      m_hwFrame->width = m_codecCtx->width;
      m_hwFrame->height = m_codecCtx->height;
      m_hwFrame->format = m_codecCtx->pix_fmt;
      if (av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, m_hwFrame, 0) < 0)
        return false;
    }
    if (av_hwframe_transfer_data(m_hwFrame, m_frame, 0) < 0)
      return false;
    m_hwFrame->pts = m_frame->pts;
    inputFrame = m_hwFrame;
  }

  // 7. Send to Encoder
  int ret = avcodec_send_frame(m_codecCtx, inputFrame);
  if (ret < 0)
    return false;

  ret = avcodec_receive_packet(m_codecCtx, m_packet);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    return false;
  if (ret < 0)
    return false;

  return processPacket(m_packet, output);
}

int FFmpegEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized)
    return 0;
  avcodec_send_frame(m_codecCtx, nullptr);
  int count = 0;
  while (true) {
    if (avcodec_receive_packet(m_codecCtx, m_packet) < 0)
      break;
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
  return m_type != EncoderType::Software_x264 &&
         m_type != EncoderType::Software_x265;
}

std::vector<uint8_t> FFmpegEncoder::extraData() const {
  if (!m_codecCtx || !m_codecCtx->extradata)
    return {};
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
  if (m_packet)
    av_packet_free(&m_packet);
  if (m_hwFrame)
    av_frame_free(&m_hwFrame);
  if (m_frame)
    av_frame_free(&m_frame);
  if (m_hwFramesCtx)
    av_buffer_unref(&m_hwFramesCtx);
  if (m_hwDeviceCtx)
    av_buffer_unref(&m_hwDeviceCtx);
  if (m_codecCtx)
    avcodec_free_context(&m_codecCtx);
  m_initialized = false;
}

} // namespace GameRecorder