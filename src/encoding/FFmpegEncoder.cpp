/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FFmpegEncoder.h"
#include "capture/IVideoCaptureSource.h"

#include <QDebug>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

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
  
  if (!initializeCodec(config)) {
      qWarning() << "FFmpegEncoder: Failed to initialize codec.";
      return false;
  }

  m_frame = av_frame_alloc();
  m_packet = av_packet_alloc();
  
  if (!m_frame || !m_packet) { 
      cleanup(); 
      return false; 
  }

  // Setup software fallback frame
  m_frame->format = AV_PIX_FMT_NV12; 
  m_frame->width = config.width;
  m_frame->height = config.height;

  if (av_frame_get_buffer(m_frame, 0) < 0) {
      qWarning() << "FFmpegEncoder: Failed to allocate software frame buffer.";
      cleanup();
      return false;
  }

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

  if (isHardwareEncoder()) {
    m_codecCtx->pix_fmt = m_codecName.contains("nvenc") ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
    if (!initializeHardwareContext()) return false;
  } else {
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  }

  m_codecCtx->bit_rate = static_cast<int64_t>(config.bitrate) * 1000;
  
  AVDictionary *opts = nullptr;
  if (m_codecName.contains("nvenc")) {
    av_dict_set(&opts, "preset", "p4", 0);
    av_dict_set(&opts, "tune", "ull", 0); 
    av_dict_set(&opts, "delay", "0", 0);
    av_dict_set(&opts, "rc", config.useVBR ? "vbr" : "cbr", 0);
  }

  m_codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  int ret = avcodec_open2(m_codecCtx, codec, &opts);
  av_dict_free(&opts);

  return ret >= 0;
}

bool FFmpegEncoder::initializeHardwareContext() {
    enum AVHWDeviceType type = m_codecName.contains("nvenc") ? AV_HWDEVICE_TYPE_CUDA : AV_HWDEVICE_TYPE_VAAPI;

    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, type, nullptr, nullptr, 0) < 0) return false;
    
    // Attach device context
    m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);

    m_hwFramesCtx = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!m_hwFramesCtx) return false;

    AVHWFramesContext *f = (AVHWFramesContext *)m_hwFramesCtx->data;
    f->format = m_codecCtx->pix_fmt;
    f->sw_format = AV_PIX_FMT_NV12;
    f->width = m_codecCtx->width;
    f->height = m_codecCtx->height;
    f->initial_pool_size = 20;

    if (av_hwframe_ctx_init(m_hwFramesCtx) < 0) return false;

    m_codecCtx->hw_frames_ctx = av_buffer_ref(m_hwFramesCtx);
    return true;
}

bool FFmpegEncoder::encodeFrame(const VideoFrame &frame, EncodedPacket &output) {
  if (!m_initialized || !m_codecCtx) return false;

  if (m_firstTimestamp < 0) m_firstTimestamp = frame.timestamp;
  int64_t relativeMs = (frame.timestamp - m_firstTimestamp) / 1000;

  AVFrame *finalFrame = nullptr; 
  AVFrame *tempHwFrame = nullptr; 
  AVFrame *drm_frame = nullptr;

  // 1. Attempt DMA-BUF Mapping (Zero-copy path)
  if (frame.isDmaBuf && isHardwareEncoder() && m_codecCtx->hw_frames_ctx) {
    auto *desc = (AVDRMFrameDescriptor *)av_mallocz(sizeof(AVDRMFrameDescriptor));
    if (desc) {
        // FIX 1: Duplicate FD so we own the lifetime
        desc->nb_objects = 1;
        desc->objects[0].fd = dup(frame.dmaBufFd); 
        desc->objects[0].size = frame.height * frame.dmaBufStride; 
        desc->objects[0].format_modifier = frame.drmModifier;
        
        desc->nb_layers = 1;
        desc->layers[0].format = frame.drmFormat;
        desc->layers[0].nb_planes = 1;
        desc->layers[0].planes[0].object_index = 0;
        desc->layers[0].planes[0].pitch = frame.dmaBufStride;
        desc->layers[0].planes[0].offset = frame.dmaBufOffset;

        drm_frame = av_frame_alloc();
        drm_frame->width = frame.width;
        drm_frame->height = frame.height;
        drm_frame->format = AV_PIX_FMT_DRM_PRIME;
        drm_frame->data[0] = (uint8_t *)desc;
        
        // FIX 2: Close the duplicated FD in the deleter
        drm_frame->buf[0] = av_buffer_create((uint8_t *)desc, sizeof(*desc), 
            [](void*, uint8_t* data) {
                auto *d = reinterpret_cast<AVDRMFrameDescriptor*>(data);
                if (d->objects[0].fd >= 0) close(d->objects[0].fd);
                av_free(d);
            }, nullptr, 0);

        tempHwFrame = av_frame_alloc();
        // FIX 3: Prime the frame with the hardware context
        tempHwFrame->format = m_codecCtx->pix_fmt;
        tempHwFrame->hw_frames_ctx = av_buffer_ref(m_codecCtx->hw_frames_ctx);

        if (av_hwframe_map(tempHwFrame, drm_frame, AV_HWFRAME_MAP_READ) == 0) {
            finalFrame = tempHwFrame;
        } else {
            av_frame_free(&tempHwFrame);
            tempHwFrame = nullptr;
        }
    }
  } 
  
  // 2. Software Fallback Path
  if (!finalFrame && frame.data) {
    if (av_frame_make_writable(m_frame) >= 0) {
        const uint8_t *srcData[4] = {frame.data, nullptr, nullptr, nullptr};
        int srcStride[4] = {(int)frame.stride, 0, 0, 0};
        
        if (!m_swsCtx) {
            m_swsCtx = sws_getContext(frame.width, frame.height, (AVPixelFormat)frame.pixelFormat,
                                      m_config.width, m_config.height, (AVPixelFormat)m_frame->format,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr);
        }
        
        if (m_swsCtx) {
            sws_scale(m_swsCtx, srcData, srcStride, 0, frame.height, m_frame->data, m_frame->linesize);
            
            if (isHardwareEncoder()) {
                tempHwFrame = av_frame_alloc();
                if (av_hwframe_get_buffer(m_codecCtx->hw_frames_ctx, tempHwFrame, 0) >= 0) {
                    if (av_hwframe_transfer_data(tempHwFrame, m_frame, 0) >= 0) {
                        finalFrame = tempHwFrame;
                    } else {
                        av_frame_free(&tempHwFrame);
                        tempHwFrame = nullptr;
                    }
                }
            } else {
                finalFrame = m_frame;
            }
        }
    }
  }

  bool success = false;
  if (finalFrame) {
    finalFrame->pts = av_rescale_q(relativeMs, {1, 1000}, m_codecCtx->time_base);
    int ret = avcodec_send_frame(m_codecCtx, finalFrame);
    
    if (ret >= 0) {
      m_frameCount++;
      ret = avcodec_receive_packet(m_codecCtx, m_packet);
      if (ret == 0) {
        success = processPacket(m_packet, output);
      } else if (ret == AVERROR(EAGAIN)) {
        success = true;
      }
    }
  }

  if (tempHwFrame) av_frame_free(&tempHwFrame);
  if (drm_frame) av_frame_free(&drm_frame);

  return success;
}

int FFmpegEncoder::flush(std::vector<EncodedPacket> &packets) {
  if (!m_initialized) return 0;
  avcodec_send_frame(m_codecCtx, nullptr);
  int count = 0;
  while (avcodec_receive_packet(m_codecCtx, m_packet) >= 0) {
    EncodedPacket out;
    if (processPacket(m_packet, out)) {
      packets.push_back(std::move(out));
      count++;
    }
  }
  return count;
}

bool FFmpegEncoder::processPacket(AVPacket *packet, EncodedPacket &output) {
  output.data.resize(packet->size);
  memcpy(output.data.data(), packet->data, packet->size);
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
  if (m_frame) av_frame_free(&m_frame);
  if (m_hwFramesCtx) av_buffer_unref(&m_hwFramesCtx);
  if (m_hwDeviceCtx) av_buffer_unref(&m_hwDeviceCtx);
  if (m_codecCtx) avcodec_free_context(&m_codecCtx);
  m_initialized = false;
}

} // namespace TrophyBackend