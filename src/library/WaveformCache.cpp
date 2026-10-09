/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaveformCache.h"

#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThreadPool>
#include <QtEndian>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace TrophyBackend {

namespace {

// Peaks are computed at a low mono rate: plenty for drawing,
// far less decode + resample work than full-rate stereo.
constexpr int kPeakRate = 8000;
// Max-abs is accumulated per block, then reduced to buckets —
// no reliance on container duration (which can be NOPTS).
constexpr int kBlockSamples = 256;
constexpr quint32 kCacheMagic = 0x54504b31; // "TPK1"
constexpr quint32 kCacheVersion = 1;

QString peaksPathFor(const QString &filePath) {
  const QFileInfo info(filePath);
  return info.absolutePath() + "/.thumbnails/" + info.completeBaseName() +
         ".peaks";
}

// Index of the nth audio stream, or -1.
int findAudioStream(AVFormatContext *ctx, int audioIndex) {
  int seen = 0;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      if (seen == audioIndex)
        return static_cast<int>(i);
      ++seen;
    }
  }
  return -1;
}

// Decode one audio stream of an OPEN container to normalized
// per-bucket peaks. Rewinds to the head first (callers share
// one open across tracks). Empty on any failure.
QVector<float> decodeTrackPeaks(AVFormatContext *ctx, int audioIndex) {
  QVector<float> peaks;
  const int streamIdx = findAudioStream(ctx, audioIndex);
  if (streamIdx < 0) {
    return peaks;
  }

  const AVCodecParameters *par = ctx->streams[streamIdx]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(par->codec_id);
  if (!codec) {
    return peaks;
  }

  AVCodecContext *dec = avcodec_alloc_context3(codec);
  if (!dec || avcodec_parameters_to_context(dec, par) < 0 ||
      avcodec_open2(dec, codec, nullptr) < 0) {
    avcodec_free_context(&dec);
    return peaks;
  }

  // Rewind: a shared container sits wherever the previous
  // track's pass left it.
  if (av_seek_frame(ctx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
    avcodec_free_context(&dec);
    return peaks;
  }

  SwrContext *swr = nullptr;
  AVChannelLayout outLayout;
  av_channel_layout_default(&outLayout, 1);
  if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, kPeakRate,
                           &dec->ch_layout, dec->sample_fmt, dec->sample_rate,
                           0, nullptr) != 0 ||
      swr_init(swr) != 0) {
    swr_free(&swr);
    av_channel_layout_uninit(&outLayout);
    avcodec_free_context(&dec);
    return peaks;
  }

  QVector<float> blocks;
  blocks.reserve(4096);
  float blockMax = 0.0f;
  int blockFill = 0;
  auto pushSample = [&](float s) {
    const float a = std::fabs(s);
    if (a > blockMax)
      blockMax = a;
    if (++blockFill >= kBlockSamples) {
      blocks.append(blockMax);
      blockMax = 0.0f;
      blockFill = 0;
    }
  };

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  // Output buffer sized for the worst case below (a full input
  // frame resampled down still fits with room to spare); the
  // same capacity is passed as swr's output budget — never an
  // unbounded count against a fixed buffer.
  constexpr int kOutCapacity = 4096;
  uint8_t *outBuf = nullptr;
  int outLinesize = 0;
  if (av_samples_alloc(&outBuf, &outLinesize, 1, kOutCapacity,
                       AV_SAMPLE_FMT_FLT, 0) < 0) {
    outBuf = nullptr;
  }

  bool ok = pkt && frame && outBuf;
  auto convert = [&](const uint8_t **data, int nbSamples) {
    const int got = swr_convert(swr, &outBuf, kOutCapacity, data, nbSamples);
    if (got < 0) {
      ok = false;
      return 0;
    }
    const float *samples = reinterpret_cast<const float *>(outBuf);
    for (int i = 0; i < got; ++i)
      pushSample(samples[i]);
    return got;
  };
  while (ok && av_read_frame(ctx, pkt) >= 0) {
    if (pkt->stream_index != streamIdx) {
      av_packet_unref(pkt);
      continue;
    }
    if (avcodec_send_packet(dec, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }
    av_packet_unref(pkt);
    while (ok && avcodec_receive_frame(dec, frame) == 0)
      convert((const uint8_t **)frame->data, frame->nb_samples);
  }
  // Drain the decoder, then flush the resampler: it buffers
  // input (resampling delay), so frames alone don't guarantee
  // output — a 1s file ends before swr emits anything.
  if (ok) {
    avcodec_send_packet(dec, nullptr);
    while (ok && avcodec_receive_frame(dec, frame) == 0)
      convert((const uint8_t **)frame->data, frame->nb_samples);
    while (ok && convert(nullptr, 0) > 0) {
    }
  }
  if (blockFill > 0)
    blocks.append(blockMax);

  av_freep(&outBuf);
  av_packet_free(&pkt);
  av_frame_free(&frame);
  swr_free(&swr);
  av_channel_layout_uninit(&outLayout);
  avcodec_free_context(&dec);

  if (blocks.isEmpty())
    return peaks;

  // Reduce blocks to fixed buckets, then normalize.
  peaks.resize(WaveformCache::kBuckets);
  float globalMax = 0.0f;
  for (int b = 0; b < WaveformCache::kBuckets; ++b) {
    const int from = (b * blocks.size()) / WaveformCache::kBuckets;
    const int to =
        ((b + 1) * blocks.size()) / WaveformCache::kBuckets;
    float m = 0.0f;
    for (int i = from; i < to; ++i) {
      if (blocks[i] > m)
        m = blocks[i];
    }
    peaks[b] = m;
    if (m > globalMax)
      globalMax = m;
  }
  if (globalMax > 0.0f) {
    for (float &p : peaks)
      p /= globalMax;
  }
  return peaks;
}

struct CachedPeaks {
  bool valid = false;
  QVector<QVector<float>> tracks;
};

CachedPeaks readCache(const QString &filePath, int bucketsWanted) {
  CachedPeaks out;
  const QFileInfo info(filePath);
  if (!info.exists())
    return out;
  QFile f(peaksPathFor(filePath));
  if (!f.open(QIODevice::ReadOnly))
    return out;
  QDataStream in(&f);
  in.setVersion(QDataStream::Qt_6_0);
  in.setFloatingPointPrecision(QDataStream::SinglePrecision);
  quint32 magic = 0, version = 0;
  qint64 size = 0, mtime = 0;
  qint32 trackCount = 0, buckets = 0;
  in >> magic >> version >> size >> mtime >> trackCount >> buckets;
  if (in.status() != QDataStream::Ok || magic != kCacheMagic ||
      version != kCacheVersion || buckets != bucketsWanted ||
      trackCount <= 0 || trackCount > 32 ||
      size != info.size() ||
      mtime != info.lastModified().toSecsSinceEpoch()) {
    return out;
  }
  out.tracks.resize(trackCount);
  for (int t = 0; t < trackCount; ++t) {
    out.tracks[t].resize(bucketsWanted);
    for (int b = 0; b < bucketsWanted; ++b)
      in >> out.tracks[t][b];
  }
  if (in.status() != QDataStream::Ok)
    return CachedPeaks{};
  out.valid = true;
  return out;
}

void writeCache(const QString &filePath,
                const QVector<QVector<float>> &tracks) {
  const QFileInfo info(filePath);
  QDir dir(info.absolutePath() + "/.thumbnails");
  if (!dir.exists() && !dir.mkpath("."))
    return;
  QFile f(peaksPathFor(filePath));
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return;
  QDataStream out(&f);
  out.setVersion(QDataStream::Qt_6_0);
  out.setFloatingPointPrecision(QDataStream::SinglePrecision);
  out << kCacheMagic << kCacheVersion << info.size()
      << info.lastModified().toSecsSinceEpoch()
      << static_cast<qint32>(tracks.size())
      << static_cast<qint32>(WaveformCache::kBuckets);
  for (const auto &track : tracks) {
    for (int b = 0; b < WaveformCache::kBuckets && b < track.size(); ++b)
      out << track[b];
  }
}

QVariantList toVariant(const QVector<float> &peaks) {
  QVariantList list;
  list.reserve(peaks.size());
  for (float p : peaks)
    list.append(static_cast<double>(p));
  return list;
}

} // namespace

WaveformCache::WaveformCache(QObject *parent) : QObject(parent) {}

int WaveformCache::audioTrackCount(const QString &filePath) {
  AVFormatContext *ctx = nullptr;
  if (avformat_open_input(&ctx, filePath.toUtf8().constData(), nullptr,
                          nullptr) < 0) {
    return 0;
  }
  int count = 0;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      ++count;
  }
  avformat_close_input(&ctx);
  return count;
}

void WaveformCache::requestPeaks(const QString &filePath, int track) {
  if (filePath.isEmpty() || track < 0)
    return;

  if (const CachedPeaks cached = readCache(filePath, kBuckets);
      cached.valid) {
    QVariantList peaks;
    if (track < cached.tracks.size())
      peaks = toVariant(cached.tracks[track]);
    QMetaObject::invokeMethod(
        this,
        [this, filePath, track, peaks]() { emit peaksReady(filePath, track, peaks); },
        Qt::QueuedConnection);
    return;
  }

  auto it = m_pending.find(filePath);
  if (it != m_pending.end()) {
    it->insert(track);
    return;
  }
  m_pending[filePath] = QSet<int>{track};

  // Same QPointer discipline as Recording::loadMetadata: the
  // cache object is app-owned, but a late worker must never
  // touch a dead object. One open, sequential per-track
  // decodes (a single pass over the bytes instead of one open
  // + full decode per track), emitting each track as it
  // completes so the first lane paints ~3x sooner on
  // multi-track files.
  QPointer<WaveformCache> self(this);
  QThreadPool::globalInstance()->start([self, filePath]() {
    if (self.isNull())
      return;
    auto emitTrack = [self, filePath](int track,
                                      const QVector<float> &peaks) {
      QMetaObject::invokeMethod(
          self,
          [self, filePath, track, peaks]() {
            if (self.isNull())
              return;
            // Retire the track: this emit satisfies its
            // request, so the late-joiner pass below must
            // not re-deliver it (double emit).
            auto it = self->m_pending.find(filePath);
            if (it == self->m_pending.end() ||
                !it->contains(track))
              return;
            it->remove(track);
            if (it->isEmpty())
              self->m_pending.erase(it);
            emit self->peaksReady(filePath, track, toVariant(peaks));
          },
          Qt::QueuedConnection);
    };

    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, filePath.toUtf8().constData(), nullptr,
                            nullptr) < 0) {
      // Unreadable: still settle every owed track (empty) so
      // QML never waits forever.
      QMetaObject::invokeMethod(
          self,
          [self, filePath]() {
            if (self.isNull())
              return;
            const QSet<int> owed = self->m_pending.take(filePath);
            for (int t : owed)
              emit self->peaksReady(filePath, t, QVariantList{});
          },
          Qt::QueuedConnection);
      return;
    }

    int count = 0;
    for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
      if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        ++count;
    }
    QVector<QVector<float>> tracks;
    tracks.resize(count);
    for (int t = 0; t < count; ++t) {
      tracks[t] = decodeTrackPeaks(ctx, t);
      emitTrack(t, tracks[t]);
    }
    if (!tracks.isEmpty())
      writeCache(filePath, tracks);
    avformat_close_input(&ctx);

    // Late joiners: tracks requested mid-flight that the loop
    // already passed are served from the just-written cache.
    QMetaObject::invokeMethod(
        self,
        [self, filePath]() {
          if (self.isNull())
            return;
          const QSet<int> owed = self->m_pending.take(filePath);
          if (owed.isEmpty())
            return;
          const CachedPeaks cached = readCache(filePath, kBuckets);
          for (int t : owed) {
            QVariantList peaks;
            if (cached.valid && t >= 0 && t < cached.tracks.size())
              peaks = toVariant(cached.tracks[t]);
            emit self->peaksReady(filePath, t, peaks);
          }
        },
        Qt::QueuedConnection);
  });
}

QString WaveformCache::trackLabel(int track, int trackCount) {
  if (trackCount <= 1)
    return QStringLiteral("Audio");
  // Recorder order (RecordingSession): game, voice, mic.
  switch (track) {
  case 0:
    return QStringLiteral("Game");
  case 1:
    return QStringLiteral("Voice");
  case 2:
    return QStringLiteral("Mic");
  default:
    break;
  }
  return QStringLiteral("Track %1").arg(track + 1);
}

} // namespace TrophyBackend
