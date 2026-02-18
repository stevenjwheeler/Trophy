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

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace GameRecorder {

std::vector<EncoderInfo> EncoderFactory::detectAvailableEncoders() {
  static std::vector<EncoderInfo> s_cache;
  if (!s_cache.empty())
    return s_cache;

  std::vector<EncoderInfo> encoders;

  // Check NVENC H.264
  if (probeNvenc()) {
    encoders.push_back({
        IEncoder::EncoderType::NVENC_H264, "nvenc_h264", "NVENC H.264", true,
        10 // Highest priority
    });

    encoders.push_back({IEncoder::EncoderType::NVENC_HEVC, "nvenc_hevc",
                        "NVENC HEVC", true, 11});
  }

  // Check VA-API
  if (probeVaapi()) {
    encoders.push_back({IEncoder::EncoderType::VAAPI_H264, "vaapi_h264",
                        "VA-API H.264", true, 20});

    encoders.push_back({IEncoder::EncoderType::VAAPI_HEVC, "vaapi_hevc",
                        "VA-API HEVC", true, 21});
  }

  // Software x264 is always available as fallback
  if (probeX264()) {
    encoders.push_back(
        {IEncoder::EncoderType::Software_x264, "x264", "x264", false, 100});
  }

  std::sort(encoders.begin(), encoders.end());

  qInfo() << "Detected" << encoders.size() << "available encoders:";
  for (const auto &enc : encoders) {
    qInfo() << "  -" << enc.name << "(" << enc.description << ")";
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
  case IEncoder::EncoderType::VAAPI_H264:
    codecName = "h264_vaapi";
    break;
  case IEncoder::EncoderType::VAAPI_HEVC:
    codecName = "hevc_vaapi";
    break;
  case IEncoder::EncoderType::Software_x264:
    codecName = "libx264";
    break;
  case IEncoder::EncoderType::Software_x265:
    codecName = "libx265";
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
  if (name == "vaapi_h264" || name == "vaapi") {
    return create(IEncoder::EncoderType::VAAPI_H264);
  }
  if (name == "vaapi_hevc") {
    return create(IEncoder::EncoderType::VAAPI_HEVC);
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
        qInfo() << "Selected hardware encoder:" << enc.name;
        return create(enc.type);
      }
    }
  }

  // Use first available (best priority)
  qInfo() << "Selected encoder:" << encoders[0].name;
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
  if (!codec) {
    return false;
  }

  // Try to create context to verify GPU is available
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    return false;
  }

  // Set minimal parameters
  ctx->width = 1920;
  ctx->height = 1080;
  ctx->time_base = {1, 60};
  ctx->framerate = {60, 1};
  ctx->pix_fmt = AV_PIX_FMT_NV12;
  ctx->bit_rate = 6000000;

  int ret = avcodec_open2(ctx, codec, nullptr);
  avcodec_free_context(&ctx);

  if (ret < 0) {
    qDebug() << "NVENC probe failed: codec context could not be opened";
    return false;
  }

  qInfo() << "NVENC is available";
  return true;
#else
  return false;
#endif
}

bool EncoderFactory::probeVaapi() {
#ifdef HAVE_VAAPI
  const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
  if (!codec)
    return false;

  // Open a VA-API device (nullptr = auto-enumerate /dev/dri/renderD*)
  AVBufferRef *hwDeviceCtx = nullptr;
  int ret = av_hwdevice_ctx_create(&hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI,
                                   nullptr, nullptr, 0);
  if (ret < 0) {
    qDebug() << "VA-API probe failed: no usable VA-API device found";
    qInfo() << "VA-API is not available";
    return false;
  }

  // Probe encoding capability: actually try to open the codec.
  // A device context alone does not guarantee encoding support
  // (e.g. nvidia-vaapi-driver provides decoding only).
  AVCodecContext *ctx = avcodec_alloc_context3(codec);
  if (!ctx) {
    av_buffer_unref(&hwDeviceCtx);
    return false;
  }

  ctx->width = 1920;
  ctx->height = 1080;
  ctx->time_base = {1, 60};
  ctx->framerate = {60, 1};
  ctx->pix_fmt = AV_PIX_FMT_VAAPI;
  ctx->bit_rate = 6000000;
  ctx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);

  AVBufferRef *hwFramesCtx = av_hwframe_ctx_alloc(hwDeviceCtx);
  if (!hwFramesCtx) {
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDeviceCtx);
    return false;
  }

  AVHWFramesContext *framesCtx =
      reinterpret_cast<AVHWFramesContext *>(hwFramesCtx->data);
  framesCtx->format = AV_PIX_FMT_VAAPI;
  framesCtx->sw_format = AV_PIX_FMT_NV12;
  framesCtx->width = 1920;
  framesCtx->height = 1080;
  framesCtx->initial_pool_size = 4;

  ret = av_hwframe_ctx_init(hwFramesCtx);
  if (ret < 0) {
    av_buffer_unref(&hwFramesCtx);
    avcodec_free_context(&ctx);
    av_buffer_unref(&hwDeviceCtx);
    qDebug() << "VA-API probe failed: could not initialise hw frames context";
    qInfo() << "VA-API is not available";
    return false;
  }

  ctx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
  av_buffer_unref(&hwFramesCtx);

  ret = avcodec_open2(ctx, codec, nullptr);
  avcodec_free_context(&ctx); // releases ctx->hw_device_ctx and hw_frames_ctx
  av_buffer_unref(&hwDeviceCtx);

  if (ret < 0) {
    qDebug() << "VA-API probe failed: codec could not be opened (no encoding "
                "entrypoints)";
    qInfo() << "VA-API is not available";
    return false;
  }

  qInfo() << "VA-API is available";
  return true;
#else
  return false;
#endif
}

bool EncoderFactory::probeX264() {
  const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
  return codec != nullptr;
}

} // namespace GameRecorder
