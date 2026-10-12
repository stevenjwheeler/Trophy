/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ReplayClipper.h"

#include "TrophyMetadata.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <cmath>

#include <limits>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
}

namespace TrophyBackend {

namespace {

// Shared ownership: inputs live in a QList (copyable elements), the last
// copy closes the context exactly once.
using InputContext = std::shared_ptr<AVFormatContext>;
inline InputContext wrapInput(AVFormatContext *ctx) {
  return InputContext(ctx, [](AVFormatContext *c) {
    if (c)
      avformat_close_input(&c);
  });
}

struct OutputCloser {
  AVFormatContext *ctx = nullptr;
  ~OutputCloser() {
    if (!ctx)
      return;
    if (!(ctx->oformat->flags & AVFMT_NOFILE))
      avio_closep(&ctx->pb);
    avformat_free_context(ctx);
  }
};

// Packet timestamps in seconds on a stream's time base.
inline double pktPtsSec(const AVPacket *pkt, const AVRational &tb) {
  if (pkt->pts == AV_NOPTS_VALUE)
    return -1.0;
  return pkt->pts * av_q2d(tb);
}

// One input file plus its start offset in seconds relative to the first
// input. Layout contract (matches our muxer): stream 0 is video, the rest
// is audio. Each pass opens its own handles: seeking back to zero proved
// unreliable (reads resumed mid-file), so the scan and copy passes each
// open the files fresh.
struct CutInput {
  QString path;
  double offset = 0.0;
};

AVFormatContext *openCutInput(const CutInput &in, QString *errMsg) {
  AVFormatContext *ctx = nullptr;
  if (avformat_open_input(&ctx, in.path.toUtf8().constData(), nullptr,
                          nullptr) < 0) {
    if (errMsg)
      *errMsg = QString("cannot open input %1").arg(in.path);
    return nullptr;
  }
  if (avformat_find_stream_info(ctx, nullptr) < 0) {
    avformat_close_input(&ctx);
    if (errMsg)
      *errMsg = QString("cannot probe input %1").arg(in.path);
    return nullptr;
  }
  return ctx;
}

// All inputs must share the first input's stream layout. Handles close
// immediately via shared ownership.
bool validateLayout(const QList<CutInput> &inputs, unsigned *nbStreams,
                    QString *errMsg) {
  QList<InputContext> probes;
  for (const CutInput &in : inputs) {
    AVFormatContext *ctx = openCutInput(in, errMsg);
    if (!ctx)
      return false;
    probes.push_back(wrapInput(ctx));
  }
  AVFormatContext *first = probes.front().get();
  if (first->nb_streams < 1) {
    if (errMsg)
      *errMsg = QStringLiteral("first input has no streams");
    return false;
  }
  if (first->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
    if (errMsg)
      *errMsg = QStringLiteral("stream 0 is not video");
    return false;
  }
  for (const InputContext &p : probes) {
    if (p->nb_streams != first->nb_streams) {
      if (errMsg)
        *errMsg = QStringLiteral("input stream layout mismatch");
      return false;
    }
  }
  *nbStreams = first->nb_streams;
  return true;
}

// Global video end (max video pts across inputs, seconds) plus every video
// keyframe time (valid DTS only — file-head packets often lack one and a
// DTS-less start breaks the mux clock). Each pass opens its own handles.
bool scanKeys(const QList<CutInput> &inputs, QVector<double> *keyTimes,
              QVector<double> *keyDts, double *globalEnd, QString *errMsg) {
  double end = -1.0;
  QVector<double> keys;
  QVector<double> dts;
  for (const CutInput &in : inputs) {
    AVFormatContext *ctx = openCutInput(in, errMsg);
    if (!ctx)
      return false;
    InputContext holder = wrapInput(ctx);
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
      if (errMsg)
        *errMsg = QStringLiteral("cannot allocate scan packet");
      return false;
    }
    int readResult = 0;
    while ((readResult = av_read_frame(ctx, pkt)) >= 0) {
      if (pkt->stream_index == 0) {
        const double t = in.offset + pktPtsSec(pkt, ctx->streams[0]->time_base);
        if (t > end)
          end = t;
        if (t >= 0 && (pkt->flags & AV_PKT_FLAG_KEY) &&
            pkt->dts != AV_NOPTS_VALUE) {
          keys.push_back(t);
          dts.push_back(in.offset + pkt->dts *
                                       av_q2d(ctx->streams[0]->time_base));
        }
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (readResult != AVERROR_EOF) {
      if (errMsg)
        *errMsg = QStringLiteral("error reading input while scanning keyframes");
      return false;
    }
  }
  *keyTimes = keys;
  *keyDts = dts;
  *globalEnd = end;
  return true;
}

double dtsForKey(const QVector<double> &keyTimes,
                 const QVector<double> &keyDts, double keyTime) {
  for (qsizetype i = 0; i < keyTimes.size() && i < keyDts.size(); ++i) {
    if (keyTimes.at(i) == keyTime)
      return keyDts.at(i);
  }
  return keyTime;
}

// Stream copy must start on a keyframe. Cutting at the first keyframe
// *after* the cut undershoots the window by up to a GOP (a 30s clip saved
// as 28s with 2s GOPs); cutting at the last keyframe *at or before* it
// overshoots by at most a GOP instead, keeping everything asked for.
// Falls back forward when the cut precedes the first keyframe. Returns
// negative when there is no usable keyframe at all.
double pickCutKey(const QVector<double> &keyTimes, double cutSec) {
  double lastBefore = -1.0, firstAfter = -1.0;
  for (const double kt : keyTimes) {
    if (kt <= cutSec && kt > lastBefore)
      lastBefore = kt;
    if (kt >= cutSec && (firstAfter < 0 || kt < firstAfter))
      firstAfter = kt;
  }
  return (lastBefore >= 0) ? lastBefore : firstAfter;
}

// Copies everything from the cut keyframe on (closed GOPs keep PTS order
// within the kept range), dropping packets past endSec on every stream
// (+infinity disables the end bound). Timestamps are rebased against the
// selected video keyframe's global DTS for every stream. A shared anchor
// preserves late-starting audio offsets as well as each stream's PTS-DTS
// relationship. Output DTS must never step
// backwards — older packets are overlap duplicates from rotation.
bool writeCut(const QList<CutInput> &inputs, unsigned nbStreams, double cutKey,
              double timelineOriginSec, double endSec,
              const QString &outputPath, QString *error,
              double *keptSeconds, double cutSecForLog) {
  auto fail = [&](const QString &msg) {
    qWarning() << "ReplayClipper:" << msg;
    if (error)
      *error = msg;
    return false;
  };

  // Create the output with copied stream layouts.
  AVFormatContext *outCtx = nullptr;
  if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", nullptr) <
          0 ||
      !outCtx) {
    return fail("cannot create matroska context");
  }
  OutputCloser outHolder{outCtx};
  {
    QString msg;
    AVFormatContext *firstCtx = openCutInput(inputs.front(), &msg);
    if (!firstCtx)
      return fail(msg);
    InputContext firstHolder = wrapInput(firstCtx);
    // Carry the input's container tags (notably title=<game>) so the
    // clip names itself after the game instead of its filename. Must
    // precede the header write. Then stamp the clip's own identity: its
    // save moment (not the source's start), its kind and the recorder
    // version — the kind/date/time tags survive renames, unlike the
    // _Replay filename suffix and timestamp.
    av_dict_copy(&outCtx->metadata, firstCtx->metadata, 0);
    const QDateTime nowLocal = QDateTime::currentDateTime();
    av_dict_set(&outCtx->metadata, "trophy-kind", "clip", 0);
    av_dict_set(&outCtx->metadata, "trophy-version",
                trophyVersion().toUtf8().constData(), 0);
    av_dict_set(&outCtx->metadata, "trophy-date",
                trophyDateString(nowLocal).toUtf8().constData(), 0);
    av_dict_set(&outCtx->metadata, "trophy-time",
                trophyTimeString(nowLocal).toUtf8().constData(), 0);
    av_dict_set(&outCtx->metadata, "creation_time",
                QDateTime::currentDateTimeUtc()
                    .toString(Qt::ISODate)
                    .toUtf8()
                    .constData(),
                0);
    for (unsigned s = 0; s < nbStreams; ++s) {
      AVStream *inStream = firstCtx->streams[s];
      AVStream *outStream = avformat_new_stream(outCtx, nullptr);
      if (!outStream)
        return fail("cannot create output stream");
      if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) < 0)
        return fail("cannot copy codec parameters");
      outStream->codecpar->codec_tag = 0;
      outStream->time_base = inStream->time_base;
    }
  }
  if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&outCtx->pb, outputPath.toUtf8().constData(),
                  AVIO_FLAG_WRITE) < 0) {
      return fail(QString("cannot open output %1").arg(outputPath));
    }
  }
  if (avformat_write_header(outCtx, nullptr) < 0)
    return fail("cannot write header");

  const bool hasEnd = endSec < std::numeric_limits<double>::infinity();
  bool cutFound = false;
  double firstVideoT = -1.0, lastVideoT = -1.0;
  QVector<int64_t> maxDtsOut(nbStreams, -1);
  QVector<int> keptPerStream(nbStreams, 0);
  int keptPackets = 0;
  const int64_t timelineOriginUs =
      static_cast<int64_t>(std::llround(timelineOriginSec * AV_TIME_BASE));

  for (const CutInput &in : inputs) {
    QString msg;
    AVFormatContext *ctx = openCutInput(in, &msg);
    if (!ctx)
      return fail(msg);
    InputContext holder = wrapInput(ctx);
    const int64_t offsetUs =
        static_cast<int64_t>(in.offset * AV_TIME_BASE + 0.5);
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
      return fail("cannot allocate input packet");
    int readResult = 0;
    while ((readResult = av_read_frame(ctx, pkt)) >= 0) {
      const int si = pkt->stream_index;
      const AVRational inTb = ctx->streams[si]->time_base;
      const AVRational outTb = outCtx->streams[si]->time_base;
      if (pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE) {
        av_packet_unref(pkt);
        continue;
      }
      const int64_t ptsUs = av_rescale_q(pkt->pts, inTb, AV_TIME_BASE_Q);
      const int64_t dtsUs = av_rescale_q(pkt->dts, inTb, AV_TIME_BASE_Q);
      const double t = (ptsUs + offsetUs) / double(AV_TIME_BASE);
      if (si == 0) {
        // cutKey was collected with the identical timestamp math, so the
        // cut keyframe matches exactly (no epsilon needed).
        if (!cutFound) {
          if ((pkt->flags & AV_PKT_FLAG_KEY) && pkt->dts != AV_NOPTS_VALUE &&
              t >= cutKey)
            cutFound = true;
          else {
            av_packet_unref(pkt);
            continue;
          }
        } else if (hasEnd && t > endSec) {
          av_packet_unref(pkt);
          continue;
        }
      } else if (!cutFound || t < cutKey || (hasEnd && t > endSec)) {
        av_packet_unref(pkt);
        continue;
      }
      AVPacket *opkt = av_packet_alloc();
      if (!opkt || av_packet_ref(opkt, pkt) < 0) {
        av_packet_free(&opkt);
        av_packet_unref(pkt);
        av_packet_free(&pkt);
        return fail("cannot copy input packet");
      }
      const int64_t packetDtsUs = dtsUs + offsetUs;
      const int64_t dd = packetDtsUs - timelineOriginUs;
      const AVRational litSrc{1, 1000000};
      const AVRational litDst{1, 1000};
      opkt->pts =
          av_rescale_q(ptsUs + offsetUs - timelineOriginUs, AV_TIME_BASE_Q, outTb);
      opkt->dts = av_rescale_q(dd, litSrc, litDst);
      opkt->duration = av_rescale_q(pkt->duration, inTb, outTb);
      opkt->stream_index = si;
      // NOTE: opkt is owned by the muxer after a successful write —
      // never touch its fields past this point. Cache what we need.
      const int64_t outDts = opkt->dts;
      if (outDts <= maxDtsOut[si]) {
        // Overlap duplicate from segment rotation: already written.
        av_packet_free(&opkt);
      } else if (av_interleaved_write_frame(outCtx, opkt) < 0) {
        av_packet_free(&opkt);
        av_packet_unref(pkt);
        av_packet_free(&pkt);
        return fail("mux error");
      } else {
        // av_interleaved_write_frame takes ownership on success.
        maxDtsOut[si] = outDts;
        ++keptPackets;
        ++keptPerStream[si];
        if (si == 0) {
          if (firstVideoT < 0)
            firstVideoT = t;
          lastVideoT = t;
        }
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (readResult != AVERROR_EOF)
      return fail("error reading input while writing clip");
  }

  if (keptPackets == 0)
    return fail("window matched no packets");

  if (keptSeconds)
    *keptSeconds = (firstVideoT >= 0 && lastVideoT >= firstVideoT)
                       ? lastVideoT - firstVideoT
                       : 0.0;

  if (av_write_trailer(outCtx) < 0)
    return fail("cannot write trailer");
  if (outCtx->pb)
    avio_flush(outCtx->pb);
  return true;
}

} // namespace

bool ReplayClipper::saveTail(const QList<ReplaySegment> &segments,
                             double windowSeconds, const QString &outputPath,
                             QString *error, double *keptSeconds) {
  auto fail = [&](const QString &msg) {
    qWarning() << "ReplayClipper:" << msg;
    if (error)
      *error = msg;
    return false;
  };

  if (segments.isEmpty())
    return fail("no segments");
  if (windowSeconds <= 0)
    return fail("window must be positive");
  if (outputPath.isEmpty())
    return fail("no output path");

  QList<CutInput> inputs;
  for (const ReplaySegment &seg : segments)
    inputs.push_back({seg.path, seg.startOffsetSec});

  unsigned nbStreams = 0;
  {
    QString msg;
    if (!validateLayout(inputs, &nbStreams, &msg))
      return fail(msg);
  }

  QVector<double> keyTimes;
  QVector<double> keyDts;
  double globalEnd = -1.0;
  {
    QString msg;
    if (!scanKeys(inputs, &keyTimes, &keyDts, &globalEnd, &msg))
      return fail(msg);
  }
  if (globalEnd < 0)
    return fail("segments contain no video packets");

  const double cutSec = globalEnd - windowSeconds;
  const double cutKey = pickCutKey(keyTimes, cutSec);
  if (cutKey < 0)
    return fail("segments contain no usable keyframe");

  return writeCut(inputs, nbStreams, cutKey,
                  dtsForKey(keyTimes, keyDts, cutKey),
                  std::numeric_limits<double>::infinity(), outputPath, error,
                  keptSeconds, cutSec);
}

bool ReplayClipper::saveRange(const QString &sourcePath, double startSeconds,
                              double endSeconds, const QString &outputPath,
                              QString *error, double *keptSeconds) {
  auto fail = [&](const QString &msg) {
    qWarning() << "ReplayClipper:" << msg;
    if (error)
      *error = msg;
    return false;
  };

  if (sourcePath.isEmpty())
    return fail("no source file");
  if (!QFileInfo::exists(sourcePath))
    return fail(QString("source file not found"));
  if (startSeconds < 0)
    return fail("range start must be non-negative");
  if (!(endSeconds > startSeconds))
    return fail("range end must be after the start");
  if (outputPath.isEmpty())
    return fail("no output path");

  const QList<CutInput> inputs{{sourcePath, 0.0}};

  unsigned nbStreams = 0;
  {
    QString msg;
    if (!validateLayout(inputs, &nbStreams, &msg))
      return fail(msg);
  }

  QVector<double> keyTimes;
  QVector<double> keyDts;
  double globalEnd = -1.0;
  {
    QString msg;
    if (!scanKeys(inputs, &keyTimes, &keyDts, &globalEnd, &msg))
      return fail(msg);
  }
  if (globalEnd < 0)
    return fail("source contains no video packets");

  const double cutKey = pickCutKey(keyTimes, startSeconds);
  if (cutKey < 0)
    return fail("source contains no usable keyframe");

  return writeCut(inputs, nbStreams, cutKey,
                  dtsForKey(keyTimes, keyDts, cutKey), endSeconds, outputPath,
                  error, keptSeconds, startSeconds);
}

} // namespace TrophyBackend
