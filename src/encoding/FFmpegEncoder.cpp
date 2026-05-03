/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FFmpegEncoder.h"
#include "capture/IVideoCaptureSource.h"

#include <QDebug>
#include <unistd.h> // Required for sysconf(_SC_PAGESIZE)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <sys/mman.h> // mmap / munmap for DMA-BUF CPU readback

namespace TrophyBackend {

FFmpegEncoder::FFmpegEncoder(EncoderType type, const QString &codecName,
                             QObject *parent)
    : IEncoder(parent), m_type(type), m_codecName(codecName) {
  switch (type) {
  case EncoderType::NVENC_H264: m_name = "NVENC H.264"; break;
  case EncoderType::NVENC_HEVC: m_name = "NVENC HEVC"; break;
  case EncoderType::NVENC_AV1:  m_name = "NVENC AV1"; break;
  case EncoderType::VAAPI_H264: m_name = "VA-API H.264"; break;
  case EncoderType::VAAPI_HEVC: m_name = "VA-API HEVC"; break;
  case EncoderType::VAAPI_AV1:  m_name = "VA-API AV1"; break;
  case EncoderType::Software_x264: m_name = "x264"; break;
  }
}

FFmpegEncoder::~FFmpegEncoder() { cleanup(); }

bool FFmpegEncoder::initialize(const EncoderConfig &config) {
  if (m_initialized) cleanup();

  m_config = config;
  m_lastInputWidth = 0;
  m_lastInputHeight = 0;
  m_swsInputFmt = AV_PIX_FMT_NONE;

  if (!initializeCodec(config)) return false;

  m_frame = av_frame_alloc();
  if (!m_frame) { cleanup(); return false; }

  m_frame->format = (isHardwareEncoder()) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  m_frame->width = config.width;
  m_frame->height = config.height;

  if (av_frame_get_buffer(m_frame, 0) < 0) return false;

  m_packet = av_packet_alloc();
  if (!m_packet) { cleanup(); return false; }

  m_frameCount = 0;
  m_firstTimestamp = -1; 
  m_initialized = true;

  return true;
}

bool FFmpegEncoder::initializeCodec(const EncoderConfig &config) {
  const AVCodec *codec = avcodec_find_encoder_by_name(m_codecName.toUtf8().constData());
  if (!codec) return false;

  m_codecCtx = avcodec_alloc_context3(codec);
  if (!m_codecCtx) return false;

  m_codecCtx->width = config.width;
  m_codecCtx->height = config.height;
  m_codecCtx->time_base = {1, 1000};
  m_codecCtx->framerate = {config.fps, 1};
  m_codecCtx->gop_size = config.fps * 2;

  if (isHardwareEncoder() || config.preset.toLower() == "ultrafast") {
    m_codecCtx->max_b_frames = 0;
  } else {
    m_codecCtx->max_b_frames = 2;
  }

  int64_t bps = static_cast<int64_t>(config.bitrate) * 1000;
  m_codecCtx->bit_rate = bps;
  
  if (m_codecName != "libsvtav1") {
    m_codecCtx->rc_max_rate = config.useVBR ? static_cast<int64_t>(config.maxBitrate) * 1000 : bps;
    m_codecCtx->rc_buffer_size = m_codecCtx->rc_max_rate * 2;
  }

  if (isHardwareEncoder()) {
    if (m_codecName.contains("vaapi")) {
      m_codecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
      if (!initializeHardwareContext()) return false;
    } else {
      m_codecCtx->pix_fmt = AV_PIX_FMT_NV12;
    }
  } else {
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  }

  AVDictionary *opts = nullptr;
  if (m_codecName.contains("nvenc")) {
    av_dict_set(&opts, "rc", config.useVBR ? "vbr" : "cbr", 0);
    av_dict_set(&opts, "preset", "p4", 0);
  } else if (!m_codecName.contains("vaapi")) {
    av_dict_set(&opts, "preset", config.preset.toLower().toUtf8().constData(), 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);
  }

  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int ret = avcodec_open2(m_codecCtx, codec, &opts);
  av_dict_free(&opts);

  return ret >= 0;
}

bool FFmpegEncoder::initializeHardwareContext() {
#ifdef HAVE_VAAPI
  if (av_hwdevice_ctx_create(&m_drmDeviceCtx, AV_HWDEVICE_TYPE_DRM, nullptr, nullptr, 0) < 0) {
    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0) return false;
  } else {
    if (av_hwdevice_ctx_create_derived(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, m_drmDeviceCtx, 0) < 0) {
      av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    }
  }
  m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
  m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
  AVHWFramesContext *f = (AVHWFramesContext *)m_hwFramesCtx->data;
  f->format = AV_PIX_FMT_VAAPI; f->sw_format = AV_PIX_FMT_NV12;
  f->width = m_codecCtx->width; f->height = m_codecCtx->height;
  f->initial_pool_size = 20;
  if (av_hwframe_ctx_init(m_hwFramesCtx) < 0) return false;
  m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
  return true;
#else
  return false;
#endif
}

bool FFmpegEncoder::encodeFrame(const VideoFrame &frame, EncodedPacket &output) {
  if (!m_initialized) return false;

  // 1. Establish the relative baseline
  if (m_firstTimestamp < 0) m_firstTimestamp = frame.timestamp;
  int64_t relativeMs = (frame.timestamp - m_firstTimestamp) / 1000;

  // 2. Internal sequential PTS for encoder state
  const int64_t internalPts = m_frameCount;

  AVFrame *inputFrame = nullptr;

  if (frame.isDmaBuf && m_codecCtx->hw_frames_ctx) {
    auto *drmDesc = new AVDRMFrameDescriptor{};
    drmDesc->nb_objects = 1;
    drmDesc->objects[0].fd = frame.dmaBufFd;
    drmDesc->objects[0].format_modifier = frame.drmModifier;
    drmDesc->nb_layers = 1;
    drmDesc->layers[0].format = frame.drmFormat;
    drmDesc->layers[0].nb_planes = 1;
    drmDesc->layers[0].planes[0].offset = frame.dmaBufOffset;
    drmDesc->layers[0].planes[0].pitch = frame.dmaBufStride;

    AVFrame *drmFrame = av_frame_alloc();
    drmFrame->format = AV_PIX_FMT_DRM_PRIME;
    drmFrame->width = frame.width;
    drmFrame->height = frame.height;
    drmFrame->data[0] = (uint8_t *)drmDesc;
    drmFrame->buf[0] = av_buffer_create(drmFrame->data[0], sizeof(*drmDesc),
        [](void *, uint8_t *p) { delete (AVDRMFrameDescriptor *)p; }, nullptr, 0);

    if (!m_hwFrame) m_hwFrame = av_frame_alloc();
    if (av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, m_hwFrame, 0) >= 0) {
      if (av_hwframe_map(m_hwFrame, drmFrame, AV_HWFRAME_MAP_READ) >= 0) {
        m_hwFrame->pts = internalPts;
        inputFrame = m_hwFrame;
      }
    }
    av_frame_free(&drmFrame);
  }

  if (!inputFrame && frame.isDmaBuf) {
    long ps = sysconf(_SC_PAGESIZE);
    off_t aligned_off = (frame.dmaBufOffset / ps) * ps;
    size_t adj = frame.dmaBufOffset - aligned_off;
    size_t map_sz = static_cast<size_t>(frame.dmaBufStride) * frame.height + adj;

    void *mapped = mmap(nullptr, map_sz, PROT_READ, MAP_SHARED, frame.dmaBufFd, aligned_off);
    if (mapped != MAP_FAILED) {
      if (av_frame_make_writable(m_frame) >= 0) {
        const uint8_t *srcData[4] = {(uint8_t *)mapped + adj, nullptr, nullptr, nullptr};
        int srcStride[4] = {frame.dmaBufStride, 0, 0, 0};
        
        if (!m_swsCtx) {
           m_swsCtx = sws_getContext(frame.width, frame.height, AV_PIX_FMT_BGR0,
                                     m_config.width, m_config.height, (AVPixelFormat)m_frame->format,
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);
        }
        sws_scale(m_swsCtx, srcData, srcStride, 0, frame.height, m_frame->data, m_frame->linesize);
        m_frame->pts = internalPts;
        inputFrame = m_frame;
      }
      munmap(mapped, map_sz);
    }
  }

  if (!inputFrame) {
      if (av_frame_make_writable(m_frame) >= 0) {
          const uint8_t *srcData[4] = {frame.data, nullptr, nullptr, nullptr};
          int srcStride[4] = {(int)frame.stride, 0, 0, 0};
          if (!m_swsCtx) {
              m_swsCtx = sws_getContext(frame.width, frame.height, AV_PIX_FMT_BGRA,
                                        m_config.width, m_config.height, (AVPixelFormat)m_frame->format,
                                        SWS_BICUBIC, nullptr, nullptr, nullptr);
          }
          sws_scale(m_swsCtx, srcData, srcStride, 0, frame.height, m_frame->data, m_frame->linesize);
          m_frame->pts = internalPts;
          inputFrame = m_frame;
      }
  }

  if (inputFrame) {
    // Rescale our millisecond relativeMs into the encoder's internal timebase
    inputFrame->pts = av_rescale_q(relativeMs, {1, 1000}, m_codecCtx->time_base); 

    int ret = avcodec_send_frame(m_codecCtx, inputFrame);
    if (inputFrame == m_hwFrame) av_frame_unref(m_hwFrame);
    
    if (ret >= 0) {
      m_frameCount++;
      ret = avcodec_receive_packet(m_codecCtx, m_packet);
      if (ret == 0) {
        // The packet now already contains the correct relativeMs in its pts/dts
        // because FFmpeg's encoder propagates the frame->pts to packet->pts.
        return processPacket(m_packet, output);
      }
    }
  }
  return false;
}

int FFmpegEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized) return 0;
  
  // Signal EOF to encoder
  avcodec_send_frame(m_codecCtx, nullptr);
  
  int count = 0;
  // Drain all remaining packets
  while (avcodec_receive_packet(m_codecCtx, m_packet) >= 0) {
    EncodedPacket output;
    
    // No rescaling needed here anymore because the encoder is 
    // spitting out the millisecond PTS we gave it earlier.
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
  
  // Rescale from encoder timebase back to our standard 1ms timebase
  output.pts = av_rescale_q(packet->pts, m_codecCtx->time_base, {1, 1000});
  output.dts = (packet->dts == AV_NOPTS_VALUE) ? output.pts : av_rescale_q(packet->dts, m_codecCtx->time_base, {1, 1000});
  output.duration = av_rescale_q(packet->duration, m_codecCtx->time_base, {1, 1000});
  
  output.isKeyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;
  output.streamIndex = 0;
  av_packet_unref(packet);
  return true;
}

bool FFmpegEncoder::isHardwareEncoder() const {
  return m_type != EncoderType::Software_x264;
}

std::vector<uint8_t> FFmpegEncoder::extraData() const {
  if (!m_codecCtx || !m_codecCtx->extradata) return {};
  return std::vector<uint8_t>(m_codecCtx->extradata, m_codecCtx->extradata + m_codecCtx->extradata_size);
}

int FFmpegEncoder::codecId() const { return m_codecCtx ? m_codecCtx->codec_id : 0; }

void FFmpegEncoder::cleanup() {
  if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
  if (m_packet) av_packet_free(&m_packet);
  if (m_hwFrame) av_frame_free(&m_hwFrame);
  if (m_frame) av_frame_free(&m_frame);
  if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
  if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
  if (m_drmDeviceCtx) av_buffer_unref(&m_drmDeviceCtx);
  if (m_codecCtx) avcodec_free_context(&m_codecCtx);
  m_initialized = false;
}

} // namespace TrophyBackend