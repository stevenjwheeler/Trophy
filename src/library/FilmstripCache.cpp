/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FilmstripCache.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QThreadPool>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {

namespace {

// Strip thumbs are small on purpose (a timeline lane, not a
// gallery); 160px keeps grabs + cache cheap.
constexpr int kThumbWidth = 160;
constexpr int kJpegQuality = 80;
constexpr int kMaxThumbs = 64;

QString thumbPathFor(const QString &filePath, int count, int index) {
  const QFileInfo info(filePath);
  return info.absolutePath() + "/.thumbnails/" + info.completeBaseName() +
         QString(".strip-%1-%2.jpg").arg(count).arg(index);
}

// Cached strip is valid when every thumb exists and the first
// is at least as new as the video (same mtime rule the
// library uses for sidecars implicitly). Only the count-keyed
// names are checked, so legacy count-less files are a miss.
bool stripCacheValid(const QString &filePath, int count) {
  const QFileInfo videoInfo(filePath);
  if (!videoInfo.exists() || count <= 0)
    return false;
  const QDateTime videoMtime = videoInfo.lastModified();
  for (int i = 0; i < count; ++i) {
    const QFileInfo thumb(thumbPathFor(filePath, count, i));
    if (!thumb.isFile() || thumb.size() <= 0 ||
        thumb.lastModified() < videoMtime)
      return false;
  }
  return true;
}

QStringList stripPaths(const QString &filePath, int count) {
  QStringList paths;
  for (int i = 0; i < count; ++i)
    paths.append(thumbPathFor(filePath, count, i));
  return paths;
}

// Deletes legacy count-less `<base>.strip-<i>.jpg` sidecars superseded by
// the count-keyed layout. Scoped strictly to this video's own strip files
// inside its .thumbnails dir — user video files are never touched, and
// other counts' caches are kept (only the single-number legacy form
// matches; `.strip-<count>-<i>.jpg` cannot match the anchored pattern).
void removeLegacyStrips(const QString &filePath) {
  const QFileInfo info(filePath);
  QDir dir(info.absolutePath() + "/.thumbnails");
  if (!dir.exists())
    return;
  const QString base = info.completeBaseName();
  const QRegularExpression legacy("^" + QRegularExpression::escape(base) +
                                  "\\.strip-\\d+\\.jpg$");
  const QStringList candidates =
      dir.entryList({base + ".strip-*.jpg"}, QDir::Files);
  for (const QString &name : candidates) {
    if (legacy.match(name).hasMatch())
      QFile::remove(dir.absoluteFilePath(name));
  }
}

// Grab one frame near `timestampUs` (backward seek lands on a
// keyframe; decoding forward from there is exact enough for
// a strip). Returns null on failure.
QImage grabFrame(AVFormatContext *ctx, AVCodecContext *dec,
                 int videoStreamIdx, int64_t timestampUs) {
  if (av_seek_frame(ctx, -1, timestampUs, AVSEEK_FLAG_BACKWARD) < 0)
    return QImage();
  avcodec_flush_buffers(dec);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  QImage out;
  if (pkt && frame) {
    auto receive = [&]() {
      if (avcodec_receive_frame(dec, frame) < 0)
        return false;
      // Decoded dimensions/format can differ from the codec context.
      // Bound the small timeline image even for extreme portrait media.
      if (frame->width <= 0 || frame->height <= 0 ||
          av_image_check_size(frame->width, frame->height, 0, nullptr) < 0)
        return true;
      const int w = kThumbWidth;
      const int h = static_cast<int>(qBound<qint64>(
          qint64(1), (qint64(frame->height) * w) / frame->width, qint64(2048)));
      QImage rgb(w, h, QImage::Format_RGB888);
      if (rgb.isNull())
        return true;
      uint8_t *dst[4] = {rgb.bits(), nullptr, nullptr, nullptr};
      int linesize[4] = {static_cast<int>(rgb.bytesPerLine()), 0, 0, 0};
      SwsContext *sws =
          sws_getContext(frame->width, frame->height,
                         static_cast<AVPixelFormat>(frame->format), w, h,
                         AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr,
                         nullptr);
      if (sws) {
        if (sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                      dst, linesize) == h)
          out = rgb;
        sws_freeContext(sws);
      }
      return true;
    };
    int readResult = 0;
    bool done = false;
    while ((readResult = av_read_frame(ctx, pkt)) >= 0) {
      if (pkt->stream_index == videoStreamIdx &&
          avcodec_send_packet(dec, pkt) >= 0)
        done = receive();
      av_packet_unref(pkt);
      if (done)
        break;
    }
    // Tiny B-delay files may not yield a frame until clean demux EOF.
    // Read errors must not be turned into successful drained thumbnails.
    if (!done && readResult == AVERROR_EOF &&
        avcodec_send_packet(dec, nullptr) >= 0)
      receive();
  }
  av_packet_free(&pkt);
  av_frame_free(&frame);
  return out;
}

} // namespace

FilmstripCache::FilmstripCache(QObject *parent) : QObject(parent) {}

void FilmstripCache::requestThumbs(const QString &filePath, int count) {
  // Same QPointer discipline as WaveformCache: app-owned, but
  // a late queued emit must never touch a dead object.
  QPointer<FilmstripCache> self(this);
  if (filePath.isEmpty() || count <= 0) {
    QMetaObject::invokeMethod(
        this,
        [self, filePath]() {
          if (self.isNull())
            return;
          emit self->filmstripReady(filePath, {});
        },
        Qt::QueuedConnection);
    return;
  }
  count = qMin(count, kMaxThumbs);

  if (stripCacheValid(filePath, count)) {
    const QStringList paths = stripPaths(filePath, count);
    QMetaObject::invokeMethod(
        this,
        [self, filePath, paths]() {
          if (self.isNull())
            return;
          emit self->filmstripReady(filePath, paths);
        },
        Qt::QueuedConnection);
    return;
  }

  const auto key = qMakePair(filePath, count);
  if (m_pending.contains(key))
    return;
  m_pending.insert(key);

  QThreadPool::globalInstance()->start([self, filePath, count]() {
    if (self.isNull())
      return;
    QStringList paths;

    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, filePath.toUtf8().constData(), nullptr,
                            nullptr) >= 0) {
      int videoStreamIdx = -1;
      const AVCodec *codec = nullptr;
      for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
          videoStreamIdx = static_cast<int>(i);
          codec = avcodec_find_decoder(ctx->streams[i]->codecpar->codec_id);
          break;
        }
      }
      AVCodecContext *dec = nullptr;
      if (videoStreamIdx >= 0 && codec) {
        dec = avcodec_alloc_context3(codec);
        if (dec &&
            (avcodec_parameters_to_context(
                 dec, ctx->streams[videoStreamIdx]->codecpar) < 0 ||
             avcodec_open2(dec, codec, nullptr) < 0)) {
          avcodec_free_context(&dec);
          dec = nullptr;
        }
      }
      const int64_t durationUs =
          (ctx->duration != AV_NOPTS_VALUE) ? ctx->duration : -1;
      if (dec && durationUs > 0) {
        QDir dir(QFileInfo(filePath).absolutePath() + "/.thumbnails");
        if (dir.exists() || dir.mkpath(".")) {
          bool allOk = true;
          // Grabs sit at each slice's center ((i + 0.5) /
          // count); the lane maps the slice itself
          // edge-to-edge (i / count .. (i + 1) / count), so
          // the strip covers the file head-to-tail. The old
          // (i + 1) / (count + 1) inset left the head
          // uncovered and every thumb misaligned.
          for (int i = 0; i < count; ++i) {
            const int64_t ts =
                ((int64_t)(2 * i + 1) * durationUs) / (2 * count);
            const QImage img =
                grabFrame(ctx, dec, videoStreamIdx, ts);
            const QString outPath = thumbPathFor(filePath, count, i);
            if (img.isNull()) {
              allOk = false;
              break;
            }
            if (!img.save(outPath, "JPEG", kJpegQuality)) {
              // A native short write can leave a nonempty truncated JPEG.
              // Never let the next request mistake it for a valid sidecar.
              QFile::remove(outPath);
              allOk = false;
              break;
            }
          }
          if (allOk) {
            // A fresh valid strip supersedes any legacy count-less files.
            removeLegacyStrips(filePath);
            paths = stripPaths(filePath, count);
          }
        }
      }
      avcodec_free_context(&dec);
      avformat_close_input(&ctx);
    }

    QMetaObject::invokeMethod(
        self,
        [self, filePath, count, paths]() {
          if (self.isNull())
            return;
          self->m_pending.remove(qMakePair(filePath, count));
          emit self->filmstripReady(filePath, paths);
        },
        Qt::QueuedConnection);
  });
}

} // namespace TrophyBackend
