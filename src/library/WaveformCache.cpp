/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaveformCache.h"
#include "RecordingSidecars.h"

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
#include <memory>
#include <vector>

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
// Max-abs is accumulated in timeline-positioned blocks, then reduced against
// one shared file span. Missing timestamps fall back to contiguous samples.
constexpr int kBlockSamples = 256;
constexpr int kMaxTracks = 32; // same bound as the sidecar schema
constexpr quint32 kCacheMagic = 0x54504b31; // "TPK1"
// v3 preserves leading silence, internal gaps and unequal stream durations.
constexpr quint32 kCacheVersion = 3;
// Keep ordinary audio legible without making near-silence look loud:
// normalization may boost by at most 20 dB (10x).
constexpr float kNormalizationFloor = 0.1f;

QString peaksPathFor(const QString &filePath) {
  const QFileInfo info(filePath);
  return info.absolutePath() + "/.thumbnails/" + info.completeBaseName() +
         ".peaks";
}

// One state per audio stream; no retained packets or full-rate sample history.
// Blocks cost one float per 256 output samples (125 bytes/second/track).
struct TrackDecoder {
  AVCodecContext *dec = nullptr;
  AVFrame *frame = nullptr;
  SwrContext *swr = nullptr;
  uint8_t *out = nullptr;
  int capacity = 0;
  bool ok = false;
  QVector<float> blocks;
  qint64 position = 0;
  qint64 end = 0;
  bool positioned = false;
  AVRational timeBase{0, 1};
  int64_t originUs = 0;
  qint64 samples = 0;
  qint64 drainSamples = 0;
  // Explicit limits bound malformed frames, resampler buffering and history.
  static constexpr int kMaxFrameSamples = 1 << 20;
  static constexpr qint64 kMaxSamples = qint64(kPeakRate) * 60 * 60 * 24;

  explicit TrackDecoder(const AVStream *stream, int64_t origin)
      : timeBase(stream->time_base), originUs(origin) {
    const auto *par = stream->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
      return;
    dec = avcodec_alloc_context3(codec);
    frame = av_frame_alloc();
    if (!dec || !frame || avcodec_parameters_to_context(dec, par) < 0 ||
        avcodec_open2(dec, codec, nullptr) < 0 || dec->sample_rate <= 0 ||
        dec->ch_layout.nb_channels <= 0)
      return;
    AVChannelLayout mono;
    av_channel_layout_default(&mono, 1);
    const int result = swr_alloc_set_opts2(
        &swr, &mono, AV_SAMPLE_FMT_FLT, kPeakRate, &dec->ch_layout,
        dec->sample_fmt, dec->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&mono);
    ok = result >= 0 && swr && swr_init(swr) >= 0;
  }

  ~TrackDecoder() {
    av_freep(&out);
    swr_free(&swr);
    av_frame_free(&frame);
    avcodec_free_context(&dec);
  }

  int convert(const uint8_t **data, int count) {
    const int wanted = swr_get_out_samples(swr, count);
    if (wanted < 0 || wanted > kMaxFrameSamples) {
      ok = false;
      return 0;
    }
    // Allocate a checked output budget sufficient for this frame. Unlike
    // fixed-small output with arbitrarily large input, this cannot accumulate
    // an unbounded full-rate backlog inside SWR.
    const int budget = qMax(4096, wanted);
    if (budget > capacity) {
      av_freep(&out);
      int linesize = 0;
      if (av_samples_alloc(&out, &linesize, 1, budget, AV_SAMPLE_FMT_FLT, 0) < 0) {
        ok = false;
        return 0;
      }
      capacity = budget;
    }
    const int got = swr_convert(swr, &out, capacity, data, count);
    if (got < 0 || samples + got > kMaxSamples) {
      ok = false;
      return 0;
    }
    samples += got;
    if (!data)
      drainSamples += got;
    const float *values = reinterpret_cast<const float *>(out);
    for (int i = 0; i < got; ++i) {
      if (!std::isfinite(values[i])) {
        ok = false;
        return 0;
      }
      const qint64 at = position++;
      if (at < 0) continue; // Encoder preroll before the common file origin.
      if (at >= kMaxSamples) {
        ok = false;
        return 0;
      }
      const auto block = at / kBlockSamples;
      if (block >= blocks.size()) blocks.resize(block + 1);
      blocks[block] = qMax(blocks[block], std::fabs(values[i]));
      end = qMax(end, at + 1);
    }
    return got;
  }

  int receive() {
    int result = AVERROR(EAGAIN);
    while (ok && (result = avcodec_receive_frame(dec, frame)) == 0) {
      if (frame->nb_samples <= 0 || frame->nb_samples > kMaxFrameSamples ||
          !frame->extended_data || frame->sample_rate != dec->sample_rate ||
          frame->format != dec->sample_fmt ||
          av_channel_layout_compare(&frame->ch_layout, &dec->ch_layout) != 0) {
        ok = false;
      } else {
        const int planes = av_sample_fmt_is_planar(dec->sample_fmt)
                               ? dec->ch_layout.nb_channels : 1;
        for (int i = 0; i < planes; ++i)
          if (!frame->extended_data[i])
            ok = false;
        if (ok) {
          if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto timestampUs = av_rescale_q(frame->best_effort_timestamp,
                                                 timeBase, AV_TIME_BASE_Q);
            // Validate before arithmetic/rescaling: malformed timestamps must
            // not allocate unbounded histories or overflow subtraction.
            const long double delta = static_cast<long double>(timestampUs) - originUs;
            if (delta < -24LL * 60 * 60 * AV_TIME_BASE ||
                delta > 24LL * 60 * 60 * AV_TIME_BASE) {
              ok = false;
              av_frame_unref(frame);
              break;
            }
            const auto deltaUs = static_cast<int64_t>(delta);
            const qint64 at = av_rescale_q(deltaUs,
                AV_TIME_BASE_Q, AVRational{1, kPeakRate}) -
                swr_get_delay(swr, kPeakRate);
            // Container quantization and sample-rate rounding are not gaps.
            // Keep SWR's buffered tail on the previous segment when a real
            // discontinuity occurs, then place this frame at its timestamp.
            if (!positioned || std::abs(at - position) > kBlockSamples) {
              if (positioned) {
                while (ok && convert(nullptr, 0) > 0) {}
                swr_close(swr);
                if (swr_init(swr) < 0) ok = false;
              }
              position = av_rescale_q(deltaUs,
                  AV_TIME_BASE_Q, AVRational{1, kPeakRate});
              positioned = true;
            }
          }
          if (ok)
          convert(const_cast<const uint8_t **>(frame->extended_data),
                  frame->nb_samples);
        }
      }
      av_frame_unref(frame);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF && result < 0)
      ok = false;
    return result;
  }

  void send(const AVPacket *packet) {
    if (!ok)
      return;
    int result = avcodec_send_packet(dec, packet);
    if (result == AVERROR(EAGAIN)) {
      receive();
      if (ok)
        result = avcodec_send_packet(dec, packet);
    }
    if (result < 0) {
      ok = false;
      return;
    }
    const int received = receive();
    // A successful NULL send must drain through decoder EOF, not merely
    // EAGAIN. Never flush SWR or publish final peaks on a demux read error.
    if (!packet && received != AVERROR_EOF)
      ok = false;
  }

  void drain() {
    if (ok) while (ok && convert(nullptr, 0) > 0) {}
  }

  QVector<float> finish(qint64 timelineSamples) {
    if (!ok)
      return {};
    if (blocks.isEmpty())
      return {};
    // Do not stretch a short/late stream to fill the file. Empty buckets are
    // actual silence in the common video/player timeline.
    const qint64 totalBlocks = (timelineSamples + kBlockSamples - 1) / kBlockSamples;
    QVector<float> peaks(WaveformCache::kBuckets);
    float globalMax = 0.0f;
    for (int b = 0; b < peaks.size(); ++b) {
      const qsizetype from = (qint64(b) * totalBlocks) / peaks.size();
      const qsizetype to = qMin<qint64>(blocks.size(),
          (qint64(b + 1) * totalBlocks) / peaks.size());
      float maximum = 0.0f;
      for (qsizetype i = from; i < to; ++i)
        maximum = qMax(maximum, blocks[i]);
      peaks[b] = maximum;
      globalMax = qMax(globalMax, maximum);
    }
    const float divisor = qMax(globalMax, kNormalizationFloor);
    for (float &peak : peaks)
      peak /= divisor;
    return peaks;
  }
};

template <typename Stats>
QVector<QVector<float>> decodeAllPeaks(
    AVFormatContext *ctx, Stats &stats,
    const std::function<int(int, qint64)> &readOverride) {
  std::vector<std::unique_ptr<TrackDecoder>> states;
  QVector<int> streamToTrack(ctx->nb_streams, -1);
  int count = 0;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i)
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      ++count;
  // Reject excessive stream fanout before allocating decoder/SWR state.
  // Requests are still settled empty by the owning job.
  if (count > kMaxTracks) {
    stats.tracks = count;
    return {};
  }
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
      continue;
    streamToTrack[i] = static_cast<int>(states.size());
    const int64_t origin = ctx->start_time == AV_NOPTS_VALUE ? 0 : ctx->start_time;
    states.push_back(std::make_unique<TrackDecoder>(ctx->streams[i], origin));
  }
  stats.tracks = static_cast<int>(states.size());
  QVector<QVector<float>> tracks(stats.tracks);
  AVPacket *packet = av_packet_alloc();
  if (!packet || states.empty()) {
    av_packet_free(&packet);
    return tracks;
  }
  ++stats.passes;
  int result;
  for (;;) {
    result = av_read_frame(ctx, packet);
    ++stats.reads;
    if (readOverride)
      result = readOverride(result, stats.reads);
    if (result < 0)
      break;
    ++stats.packets;
    if (packet->stream_index < 0 || packet->stream_index >= streamToTrack.size()) {
      result = AVERROR_INVALIDDATA;
      break;
    }
    const int track = streamToTrack[packet->stream_index];
    if (track >= 0)
      states[track]->send(packet);
    av_packet_unref(packet);
  }
  av_packet_free(&packet);
  stats.cleanEof = result == AVERROR_EOF;
  qint64 timelineSamples = 0;
  if (ctx->duration > 0 && ctx->duration != AV_NOPTS_VALUE &&
      ctx->duration <= 24LL * 60 * 60 * AV_TIME_BASE)
    timelineSamples = av_rescale_q(ctx->duration, AV_TIME_BASE_Q,
                                   AVRational{1, kPeakRate});
  for (int t = 0; t < stats.tracks; ++t) {
    auto &state = *states[t];
    if (stats.cleanEof && state.ok) {
      ++stats.decoderDrains;
      state.send(nullptr);
      if (state.ok) {
        ++stats.resamplerDrains;
        state.drain();
      }
    }
    timelineSamples = qMax(timelineSamples, state.end);
  }
  for (int t = 0; t < stats.tracks; ++t) {
    auto &state = *states[t];
    if (stats.cleanEof && state.ok)
      tracks[t] = state.finish(timelineSamples);
    stats.samples += state.samples;
    stats.drainSamples += state.drainSamples;
    stats.blocks += state.blocks.size();
    stats.blockCapacity += state.blocks.capacity();
    stats.outputCapacity += state.capacity;
    stats.maxOutputCapacity = qMax(stats.maxOutputCapacity, qint64(state.capacity));
    // Release each track's history and native state as it settles, rather
    // than holding all histories beside all 8000-bucket output vectors.
    states[t].reset();
  }
  return tracks;
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
    for (int b = 0; b < bucketsWanted; ++b) {
      in >> out.tracks[t][b];
      const float peak = out.tracks[t][b];
      if (!std::isfinite(peak) || peak < 0.0f || peak > 1.0f)
        return CachedPeaks{};
    }
  }
  if (in.status() != QDataStream::Ok)
    return CachedPeaks{};
  out.valid = true;
  return out;
}

void writeCache(const QString &filePath,
                const QVector<QVector<float>> &tracks) {
  // A short write declares full buckets in the header but stores
  // fewer floats, permanently invalidating the cache (every open
  // re-decodes). Empty/failed tracks must never be cached.
  for (const auto &track : tracks) {
    if (track.isEmpty())
      return;
  }
  const QFileInfo info(filePath);
  if (!info.isFile())
    return;
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
  f.close();
  if (!QFileInfo::exists(filePath))
    RecordingSidecars::removeForVideo(filePath);
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
    // Unknown, not empty: the caller shows a neutral "unavailable" state
    // instead of asserting the file has no audio (which would be a lie
    // for a missing/unreadable file).
    return -1;
  }
  int count = 0;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      ++count;
  }
  avformat_close_input(&ctx);
  return count;
}

void WaveformCache::requestAudioTrackCount(const QString &filePath,
                                           int requestId) {
  QPointer<WaveformCache> self(this);
  QThreadPool::globalInstance()->start([self, filePath, requestId]() {
    // The probe touches only its own container, never QObject state.
    const int count = audioTrackCount(filePath);
    if (!self)
      return;
    QMetaObject::invokeMethod(self, [self, filePath, requestId, count]() {
      if (self)
        emit self->audioTrackCountReady(filePath, requestId, count);
    }, Qt::QueuedConnection);
  });
}

void WaveformCache::requestPeaks(const QString &filePath, int track) {
  if (filePath.isEmpty() || track < 0)
    return;

  if (const auto it = m_delivering.constFind(filePath);
      it != m_delivering.cend()) {
    const QVariantList peaks = track < it->size() ? toVariant((*it)[track])
                                                : QVariantList{};
    QPointer<WaveformCache> self(this);
    QMetaObject::invokeMethod(this, [self, filePath, track, peaks]() {
      if (self)
        emit self->peaksReady(filePath, track, peaks);
    }, Qt::QueuedConnection);
    return;
  }

  if (const CachedPeaks cached = readCache(filePath, kBuckets);
      cached.valid) {
    QVariantList peaks;
    if (track < cached.tracks.size())
      peaks = toVariant(cached.tracks[track]);
    // QPointer discipline: a late queued emit must never touch a
    // dead object (matches the worker paths below).
    QPointer<WaveformCache> self(this);
    QMetaObject::invokeMethod(
        this,
        [self, filePath, track, peaks]() {
          if (self.isNull())
            return;
          emit self->peaksReady(filePath, track, peaks);
        },
        Qt::QueuedConnection);
    return;
  }

  auto it = m_pending.find(filePath);
  if (it != m_pending.end()) {
    it->insert(track);
    return;
  }
  m_pending[filePath] = QSet<int>{track};

  // The job owns its container and all decoder states. No worker accesses
  // QObject members; queued delivery is guarded independently of its lifetime.
  // One forward demux pass feeds every track. Final normalization requires
  // all of that track's samples, so no final lane is emitted before clean EOF.
  QPointer<WaveformCache> self(this);
  const auto observer = m_decodeObserver;
  const auto readOverride = m_readResultOverride;
  QThreadPool::globalInstance()->start([self, filePath, observer, readOverride]() {
    if (self.isNull())
      return;
    AVFormatContext *ctx = nullptr;
    DecodeStats stats;
    QVector<QVector<float>> tracks;
    if (avformat_open_input(&ctx, filePath.toUtf8().constData(), nullptr,
                            nullptr) >= 0) {
      tracks = decodeAllPeaks(ctx, stats, readOverride);
      if (!tracks.isEmpty())
        writeCache(filePath, tracks);
    }
    avformat_close_input(&ctx);
    if (observer)
      observer(stats);

    // Late joiners use the already-decoded data, even when native sidecar
    // writes failed. This also avoids rereading every track on the GUI
    // thread just to settle an in-flight request.
    QMetaObject::invokeMethod(
        self,
        [self, filePath, tracks]() {
          if (self.isNull())
            return;
          const QSet<int> owed = self->m_pending.take(filePath);
          if (owed.isEmpty())
            return;
          self->m_delivering.insert(filePath, tracks);
          for (int t : owed) {
            QVariantList peaks;
            if (t >= 0 && t < tracks.size())
              peaks = toVariant(tracks[t]);
            emit self->peaksReady(filePath, t, peaks);
            if (self.isNull())
              return;
          }
          self->m_delivering.remove(filePath);
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
