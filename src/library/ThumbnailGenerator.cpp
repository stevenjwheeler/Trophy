/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ThumbnailGenerator.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QtConcurrent>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {

namespace {
bool validDecodedFrame(const AVFrame *frame) {
  if (!frame || frame->width <= 0 || frame->height <= 0 ||
      av_image_check_size(frame->width, frame->height, 0, nullptr) < 0)
    return false;
  const auto format = static_cast<AVPixelFormat>(frame->format);
  const auto *descriptor = av_pix_fmt_desc_get(format);
  if (!descriptor || (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))
    return false;
  int minimumStrides[4] = {};
  const int planes = av_pix_fmt_count_planes(format);
  if (planes <= 0 || planes > 4 ||
      av_image_fill_linesizes(minimumStrides, format, frame->width) < 0)
    return false;
  for (int plane = 0; plane < planes; ++plane) {
    // Negative strides are legal; widen before negation (INT_MIN).
    const qint64 stride = frame->linesize[plane];
    if (!frame->data[plane] ||
        (stride < 0 ? -stride : stride) < minimumStrides[plane])
      return false;
    const auto *buffer = av_frame_get_plane_buffer(frame, plane);
    if (!buffer)
      return false;
    const auto base = reinterpret_cast<uintptr_t>(buffer->data);
    const auto data = reinterpret_cast<uintptr_t>(frame->data[plane]);
    if (data < base || data - base >= buffer->size)
      return false;
    const size_t offset = data - base;
    const int rows = (plane == 1 || plane == 2)
                         ? AV_CEIL_RSHIFT(frame->height, descriptor->log2_chroma_h)
                         : frame->height;
    const qint64 travel = stride * (rows - 1);
    if (travel < 0 && static_cast<uint64_t>(-travel) > offset)
      return false;
    const auto forward = travel > 0 ? static_cast<uint64_t>(travel) : 0;
    if (forward > buffer->size - offset ||
        static_cast<size_t>(minimumStrides[plane]) >
            buffer->size - offset - forward)
      return false;
  }
  return !(descriptor->flags & AV_PIX_FMT_FLAG_PAL) || frame->data[1];
}
} // namespace

ThumbnailGenerator::ThumbnailGenerator(QObject *parent)
    : QObject(parent), m_receiveFrame(&avcodec_receive_frame) {}

ThumbnailGenerator::~ThumbnailGenerator() {
  m_terminating = true;
  m_future.waitForFinished();
}

void ThumbnailGenerator::generateThumbnail(const QString &videoPath) {
  QMutexLocker locker(&m_mutex);
  m_queue.enqueue(videoPath);

  if (!m_processing) {
    m_processing = true;
    m_future = QtConcurrent::run([this]() { processQueue(); });
  }
}

void ThumbnailGenerator::processQueue() {
  while (true) {
    QString videoPath;

    {
      QMutexLocker locker(&m_mutex);
      if (m_queue.isEmpty() || m_terminating) {
        m_processing = false;
        return;
      }
      videoPath = m_queue.dequeue();
    }

    QString thumbnailPath = getThumbnailPath(videoPath);

    if (generateThumbnailSync(videoPath, thumbnailPath)) {
      emit thumbnailReady(videoPath, thumbnailPath);
    } else {
      emit thumbnailFailed(videoPath, "Failed to generate thumbnail");
    }
  }
}

QString ThumbnailGenerator::getThumbnailPath(const QString &videoPath) const {
  QFileInfo videoInfo(videoPath);
  QString thumbDir = videoInfo.absolutePath() + "/.thumbnails";

  // Ensure thumbnail directory exists
  QDir().mkpath(thumbDir);

  return thumbDir + "/" + videoInfo.completeBaseName() + ".jpg";
}

QSize ThumbnailGenerator::thumbnailSize(int width, int height) {
  if (width <= 0 || height <= 0)
    return {};

  constexpr int thumbWidth = 320;
  // Widen before multiplication: even valid positive int dimensions can
  // overflow height * thumbWidth. Reject heights QSize/FFmpeg cannot represent.
  const qint64 thumbHeight = qMax(qint64(1), qint64(height) * thumbWidth / width);
  if (thumbHeight > std::numeric_limits<int>::max())
    return {};
  return {thumbWidth, static_cast<int>(thumbHeight)};
}

bool ThumbnailGenerator::generateThumbnailSync(const QString &videoPath,
                                               const QString &outputPath) {
  AVFormatContext *formatCtx = nullptr;
  AVCodecContext *codecCtx = nullptr;
  SwsContext *swsCtx = nullptr;
  AVFrame *frame = nullptr;
  AVFrame *rgbFrame = nullptr;
  AVPacket *packet = nullptr;

  bool success = false;
  bool draining = false;

  // Open input
  if (avformat_open_input(&formatCtx, videoPath.toUtf8().constData(), nullptr,
                          nullptr) < 0) {
    qWarning() << "Failed to open video:" << videoPath;
    return false;
  }

  if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
    avformat_close_input(&formatCtx);
    return false;
  }

  // Find video stream
  int videoStreamIndex = -1;
  const AVCodec *codec = nullptr;

  for (unsigned int i = 0; i < formatCtx->nb_streams; i++) {
    if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      videoStreamIndex = static_cast<int>(i);
      codec = avcodec_find_decoder(formatCtx->streams[i]->codecpar->codec_id);
      break;
    }
  }

  if (videoStreamIndex < 0 || !codec) {
    avformat_close_input(&formatCtx);
    return false;
  }

  // Create codec context
  codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx || avcodec_parameters_to_context(
                       codecCtx,
                       formatCtx->streams[videoStreamIndex]->codecpar) < 0) {
    goto cleanup;
  }

  if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
    goto cleanup;
  }

  // Seek to 10% of duration for a representative frame
  if (formatCtx->duration != AV_NOPTS_VALUE) {
    int64_t seekTarget = formatCtx->duration / 10;
    av_seek_frame(formatCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD);
  }

  // Allocate frames
  frame = av_frame_alloc();
  rgbFrame = av_frame_alloc();
  packet = av_packet_alloc();

  if (!frame || !rgbFrame || !packet) {
    goto cleanup;
  }

  // Read until the first decoded frame. A short clip can end while the
  // decoder still buffers its only frame; drain on clean EOF, not read errors.
  while (!m_terminating) {
    const int readResult = av_read_frame(formatCtx, packet);
    if (readResult < 0) {
      if (readResult != AVERROR_EOF)
        break;
      draining = true;
    }
    if (draining || packet->stream_index == videoStreamIndex) {
      if (avcodec_send_packet(codecCtx, draining ? nullptr : packet) >= 0) {
        if (m_receiveFrame(codecCtx, frame) >= 0) {
          // Got a frame, convert to RGB and save
          const QSize size = thumbnailSize(frame->width, frame->height);
          if (size.isEmpty() || !validDecodedFrame(frame)) {
            qWarning() << "Thumbnail: invalid decoded frame"
                       << frame->width << "x" << frame->height;
            av_packet_unref(packet);
            break;
          }
          const int thumbWidth = size.width();
          const int thumbHeight = size.height();

          rgbFrame->width = thumbWidth;
          rgbFrame->height = thumbHeight;
          rgbFrame->format = AV_PIX_FMT_RGB24;

          if (av_frame_get_buffer(rgbFrame, 0) >= 0) {
            swsCtx = sws_getContext(
                frame->width, frame->height,
                static_cast<AVPixelFormat>(frame->format), thumbWidth,
                thumbHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr,
                nullptr, nullptr);

            if (swsCtx) {
              const int scaledRows = sws_scale(
                  swsCtx, frame->data, frame->linesize, 0, frame->height,
                  rgbFrame->data, rgbFrame->linesize);

              // Save as JPEG using Qt
              QImage image(rgbFrame->data[0], thumbWidth, thumbHeight,
                           rgbFrame->linesize[0], QImage::Format_RGB888);

              if (scaledRows == thumbHeight &&
                  image.save(outputPath, "JPEG", 85)) {
                success = true;
                qDebug() << "Generated thumbnail:" << outputPath;
              }

              // Freed in cleanup below (a future `continue` here
              // must not leak the context).
            }
          }

          av_packet_unref(packet);
          break;
        }
      }
    }
    av_packet_unref(packet);
    if (draining)
      break;
  }

cleanup:
  if (swsCtx)
    sws_freeContext(swsCtx);
  if (packet)
    av_packet_free(&packet);
  if (rgbFrame)
    av_frame_free(&rgbFrame);
  if (frame)
    av_frame_free(&frame);
  if (codecCtx)
    avcodec_free_context(&codecCtx);
  if (formatCtx)
    avformat_close_input(&formatCtx);

  return success;
}

} // namespace TrophyBackend
