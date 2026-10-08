/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "EncoderFactory.h"
#include "FFmpegEncoder.h"

#include <QDebug>
#include <algorithm>
#include <qdebug.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace TrophyBackend {

std::vector<EncoderInfo> EncoderFactory::detectAvailableEncoders() {
  static std::vector<EncoderInfo> s_cache;
  if (!s_cache.empty())
    return s_cache;

  std::vector<EncoderInfo> encoders;

  // Temporarily silence FFmpeg logs during hardware probing to avoid noise on unsupported systems
  int oldLevel = av_log_get_level();
  av_log_set_level(AV_LOG_QUIET);

  // Check NVENC H.264
  if (probeNvenc()) {
    encoders.push_back({
        IEncoder::EncoderType::NVENC_H264, "nvenc_h264", "NVENC H.264", true,
        10 // Highest priority
    });

    encoders.push_back({IEncoder::EncoderType::NVENC_HEVC, "nvenc_hevc",
                        "NVENC HEVC", true, 11});

    if (avcodec_find_encoder_by_name("av1_nvenc")) {
      encoders.push_back({IEncoder::EncoderType::NVENC_AV1, "nvenc_av1",
                          "NVENC AV1", true, 12});
    }
  }

  // Check VA-API
  if (probeVaapi()) {
    encoders.push_back({IEncoder::EncoderType::VAAPI_H264, "vaapi_h264",
                        "VA-API H.264", true, 20});

    encoders.push_back({IEncoder::EncoderType::VAAPI_HEVC, "vaapi_hevc",
                        "VA-API HEVC", true, 21});

    if (avcodec_find_encoder_by_name("av1_vaapi")) {
      encoders.push_back({IEncoder::EncoderType::VAAPI_AV1, "vaapi_av1",
                          "VA-API AV1", true, 22});
    }
  }

  // Software x264 is always available as fallback
  if (probeX264()) {
    encoders.push_back(
        {IEncoder::EncoderType::Software_x264, "x264", "x264", false, 100});
  }

  std::sort(encoders.begin(), encoders.end());
  av_log_set_level(oldLevel);

  qDebug() << "Detected" << encoders.size() << "available encoders:";
  for (const auto &enc : encoders) {
    qDebug() << "  -" << enc.name << "(" << enc.description << ")";
  }

  s_cache = encoders;
  return s_cache;
}

std::unique_ptr<IEncoder> EncoderFactory::create(IEncoder::EncoderType type) {
  QString codecName;

  switch (type) {
  case IEncoder::EncoderType::NVENC_H264:
    codecName = "h264_nvenc";
    break;
  case IEncoder::EncoderType::NVENC_HEVC:
    codecName = "hevc_nvenc";
    break;
  case IEncoder::EncoderType::NVENC_AV1:
    codecName = "av1_nvenc";
    break;
  case IEncoder::EncoderType::VAAPI_H264:
    codecName = "h264_vaapi";
    break;
  case IEncoder::EncoderType::VAAPI_HEVC:
    codecName = "hevc_vaapi";
    break;
  case IEncoder::EncoderType::VAAPI_AV1:
    codecName = "av1_vaapi";
    break;
  case IEncoder::EncoderType::Software_x264:
    codecName = "libx264";
    break;
  }

  auto encoder = std::make_unique<FFmpegEncoder>(type, codecName);
  return encoder;
}

std::unique_ptr<IEncoder> EncoderFactory::create(const QString &name) {
  if (name == "auto") {
    return createBest(true);
  }

  if (name == "nvenc_h264" || name == "nvenc") {
    return create(IEncoder::EncoderType::NVENC_H264);
  }
  if (name == "nvenc_hevc") {
    return create(IEncoder::EncoderType::NVENC_HEVC);
  }
  if (name == "nvenc_av1") {
    return create(IEncoder::EncoderType::NVENC_AV1);
  }
  if (name == "vaapi_h264" || name == "vaapi") {
    return create(IEncoder::EncoderType::VAAPI_H264);
  }
  if (name == "vaapi_hevc") {
    return create(IEncoder::EncoderType::VAAPI_HEVC);
  }
  if (name == "vaapi_av1") {
    return create(IEncoder::EncoderType::VAAPI_AV1);
  }
  if (name == "x264" || name == "software") {
    return create(IEncoder::EncoderType::Software_x264);
  }

  qWarning() << "Unknown encoder:" << name;
  return createBest(true);
}

std::unique_ptr<IEncoder> EncoderFactory::createBest(bool preferHardware) {
  auto encoders = detectAvailableEncoders();

  if (encoders.empty()) {
    qCritical() << "No encoders available!";
    return nullptr;
  }

  if (preferHardware) {
    // Find first hardware encoder
    for (const auto &enc : encoders) {
      if (enc.isHardware) {
        return create(enc.type);
      }
    }
  } else {
    // Explicit software request (e.g. compatibility fallback): first
    // software encoder wins.
    for (const auto &enc : encoders) {
      if (!enc.isHardware) {
        return create(enc.type);
      }
    }
  }

  // Use first available (best priority)
  return create(encoders[0].type);
}

bool EncoderFactory::isAvailable(IEncoder::EncoderType type) {
  auto encoders = detectAvailableEncoders();
  for (const auto &enc : encoders) {
    if (enc.type == type) {
      return true;
    }
  }
  return false;
}

bool EncoderFactory::probeNvenc() {
#ifdef HAVE_NVENC
  const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
  bool success = false;

  if (codec) {
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (ctx) {
      ctx->width = 1920;
      ctx->height = 1080;
      ctx->time_base = {1, 60};
      ctx->framerate = {60, 1};
      ctx->pix_fmt = AV_PIX_FMT_NV12;
      ctx->bit_rate = 6000000;

      if (avcodec_open2(ctx, codec, nullptr) >= 0) {
        success = true;
      } else {
        qDebug() << "NVENC probe failed: codec context could not be opened";
      }
      avcodec_free_context(&ctx);
    }
  }

  // Single point of truth for the Info log
  qDebug() << "NVENC is" << (success ? "available" : "not available");
  return success;
#else
  return false;
#endif
}

bool EncoderFactory::probeVaapi() {
#ifdef HAVE_VAAPI
  const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
  bool success = false;

  if (codec) {
    // Use a lambda or a simple block to manage the cleanup logic
    [&]() {
      AVBufferRef *hwDeviceCtx = nullptr;
      if (av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr,
                                 nullptr, 0) < 0) {
        qDebug() << "VA-API probe failed: no usable VA-API device found";
        return;
      }

      AVCodecContext *ctx = avcodec_alloc_context3(codec);
      if (!ctx) {
        av_buffer_unref(&hwDeviceCtx);
        return;
      }

      ctx->width = 1920;
      ctx->height = 1080;
      ctx->time_base = {1, 60};
      ctx->framerate = {60, 1};
      ctx->pix_fmt = AV_PIX_FMT_VAAPI;

      // Create a minimal hardware frames context
      AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(hwDeviceCtx);
      if (hw_frames_ref) {
        AVHWFramesContext *frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
        frames_ctx->format = AV_PIX_FMT_VAAPI;
        frames_ctx->sw_format = AV_PIX_FMT_NV12;
        frames_ctx->width = ctx->width;
        frames_ctx->height = ctx->height;
        frames_ctx->initial_pool_size = 1;

        if (av_hwframe_ctx_init(hw_frames_ref) >= 0) {
          ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
          if (avcodec_open2(ctx, codec, nullptr) >= 0) {
            success = true;
          }
        }
        av_buffer_unref(&hw_frames_ref);
      }

      avcodec_free_context(&ctx);
      av_buffer_unref(&hwDeviceCtx);
    }();
  }

  qDebug() << "VA-API is" << (success ? "available" : "not available");
  return success;
#else
  return false;
#endif
}

bool EncoderFactory::probeX264() {
  const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
  return codec != nullptr;
}

} // namespace TrophyBackend
