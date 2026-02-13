/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ThumbnailGenerator.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QtConcurrent>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace GameRecorder {

ThumbnailGenerator::ThumbnailGenerator(QObject *parent) : QObject(parent) {}

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

bool ThumbnailGenerator::generateThumbnailSync(const QString &videoPath,
                                               const QString &outputPath) {
  AVFormatContext *formatCtx = nullptr;
  AVCodecContext *codecCtx = nullptr;
  SwsContext *swsCtx = nullptr;
  AVFrame *frame = nullptr;
  AVFrame *rgbFrame = nullptr;
  AVPacket *packet = nullptr;

  bool success = false;

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
  avcodec_parameters_to_context(codecCtx,
                                formatCtx->streams[videoStreamIndex]->codecpar);

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

  // Read frames until we get a video frame
  while (!m_terminating && av_read_frame(formatCtx, packet) >= 0) {
    if (packet->stream_index == videoStreamIndex) {
      if (avcodec_send_packet(codecCtx, packet) >= 0) {
        if (avcodec_receive_frame(codecCtx, frame) >= 0) {
          // Got a frame, convert to RGB and save
          int thumbWidth = 320;
          int thumbHeight = (codecCtx->height * thumbWidth) / codecCtx->width;

          rgbFrame->width = thumbWidth;
          rgbFrame->height = thumbHeight;
          rgbFrame->format = AV_PIX_FMT_RGB24;

          if (av_frame_get_buffer(rgbFrame, 0) >= 0) {
            swsCtx = sws_getContext(codecCtx->width, codecCtx->height,
                                    codecCtx->pix_fmt, thumbWidth, thumbHeight,
                                    AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr,
                                    nullptr, nullptr);

            if (swsCtx) {
              sws_scale(swsCtx, frame->data, frame->linesize, 0,
                        codecCtx->height, rgbFrame->data, rgbFrame->linesize);

              // Save as JPEG using Qt
              QImage image(rgbFrame->data[0], thumbWidth, thumbHeight,
                           rgbFrame->linesize[0], QImage::Format_RGB888);

              if (image.save(outputPath, "JPEG", 85)) {
                success = true;
                qDebug() << "Generated thumbnail:" << outputPath;
              }

              sws_freeContext(swsCtx);
            }
          }

          av_packet_unref(packet);
          break;
        }
      }
    }
    av_packet_unref(packet);
  }

cleanup:
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

} // namespace GameRecorder
