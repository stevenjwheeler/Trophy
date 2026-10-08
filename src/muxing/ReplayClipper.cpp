/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ReplayClipper.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/timestamp.h>
}

namespace TrophyBackend {

namespace {

// Shared ownership: inputs live in a QList (copyable elements), the last
// copy closes the context exactly once.
using InputContext =
    std::shared_ptr<AVFormatContext>;
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

  // Inputs, oldest first. Layout contract (matches our muxer): stream 0
  // is video, the rest is audio. Each pass opens its own handles:
  // seeking back to zero proved unreliable (reads resumed mid-file),
  // so the end scan and the copy pass each open the files fresh.
  struct Input {
    QString path;
    double offset = 0.0;
  };
  QList<Input> inputs;
  for (const ReplaySegment &seg : segments)
    inputs.push_back({seg.path, seg.startOffsetSec});

  auto openInput = [&](const Input &in, QString *errMsg) -> AVFormatContext * {
    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, in.path.toUtf8().constData(), nullptr,
                            nullptr) < 0) {
      if (errMsg)
        *errMsg = QString("cannot open segment %1").arg(in.path);
      return nullptr;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
      avformat_close_input(&ctx);
      if (errMsg)
        *errMsg = QString("cannot probe segment %1").arg(in.path);
      return nullptr;
    }
    return ctx;
  };

  // Validate layouts; handles close immediately via shared ownership.
  unsigned nbStreams = 0;
  {
    QList<InputContext> probes;
    for (const Input &in : inputs) {
      QString msg;
      AVFormatContext *ctx = openInput(in, &msg);
      if (!ctx)
        return fail(msg);
      probes.push_back(wrapInput(ctx));
    }
    AVFormatContext *first = probes.front().get();
    if (first->nb_streams < 1)
      return fail("first segment has no streams");
    if (first->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
      return fail("stream 0 is not video");
    nbStreams = first->nb_streams;
    for (const InputContext &p : probes) {
      if (p->nb_streams != nbStreams)
        return fail("segment stream layout mismatch");
    }
  }

  // Find the global end (max video pts across segments, in seconds)
  // plus every video keyframe time (valid DTS only — file-head packets
  // often lack one and a DTS-less start breaks the mux clock). Each pass
  // opens its own handles (see above).
  double globalEnd = -1.0;
  QVector<double> keyTimes;
  for (const Input &in : inputs) {
    QString msg;
    AVFormatContext *ctx = openInput(in, &msg);
    if (!ctx)
      return fail(msg);
    InputContext holder = wrapInput(ctx);
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(ctx, pkt) >= 0) {
      if (pkt->stream_index == 0) {
        const double t = in.offset + pktPtsSec(pkt, ctx->streams[0]->time_base);
        if (t > globalEnd)
          globalEnd = t;
        if (t >= 0 && (pkt->flags & AV_PKT_FLAG_KEY) &&
            pkt->dts != AV_NOPTS_VALUE)
          keyTimes.push_back(t);
      }
      av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
  }
  if (globalEnd < 0)
    return fail("segments contain no video packets");

  const double cutSec = globalEnd - windowSeconds;
  // Stream copy must start on a keyframe. Cutting at the first keyframe
  // *after* the cut undershoots the window by up to a GOP (a 30s clip
  // saved as 28s with 2s GOPs); cutting at the last keyframe *at or
  // before* it overshoots by at most a GOP instead, keeping everything
  // the user asked for. Falls back forward when the buffer is shorter
  // than the window (cut before the first keyframe).
  double lastBefore = -1.0, firstAfter = -1.0;
  for (const double kt : keyTimes) {
    if (kt <= cutSec && kt > lastBefore)
      lastBefore = kt;
    if (kt >= cutSec && (firstAfter < 0 || kt < firstAfter))
      firstAfter = kt;
  }
  const double cutKey = (lastBefore >= 0) ? lastBefore : firstAfter;
  if (cutKey < 0)
    return fail("segments contain no usable keyframe");

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
    AVFormatContext *firstCtx = openInput(inputs.front(), &msg);
    if (!firstCtx)
      return fail(msg);
    InputContext firstHolder = wrapInput(firstCtx);
    // Carry the segment's container tags (notably title=<game>) so the
    // clip names itself after the game instead of its filename. Must
    // precede the header write. Then stamp the clip's own identity: its
    // save moment (not the segment's start) and its kind — the kind tag
    // survives renames, unlike the _Replay filename suffix.
    av_dict_copy(&outCtx->metadata, firstCtx->metadata, 0);
    av_dict_set(&outCtx->metadata, "trophy-kind", "clip", 0);
    av_dict_set(&outCtx->metadata, "creation_time",
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8().constData(), 0);
    for (unsigned s = 0; s < nbStreams; ++s) {
      AVStream *inStream = firstCtx->streams[s];
      AVStream *outStream = avformat_new_stream(outCtx, nullptr);
      if (!outStream)
        return fail("cannot create output stream");
      if (avcodec_parameters_copy(outStream->codecpar, inStream->codecpar) <
          0)
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

  // Copy pass, in file (decode) order. The cut is the last video
  // keyframe at or before cutSec (first one after it when the buffer is
  // shorter than the window); every video packet from it on is kept
  // (closed GOPs keep PTS order within the kept range). All timestamps
  // are rebased in the microsecond domain against ONE anchor per stream
  // (the first kept packet's global DTS): a single anchor preserves the
  // PTS-DTS relationship exactly, which B-frame streams require.
  // Output DTS must never step backwards — older packets are overlap
  // duplicates from rotation.
  bool cutFound = false;
  double firstVideoT = -1.0, lastVideoT = -1.0;
  QVector<int64_t> maxDtsOut(nbStreams, -1);
  QVector<int64_t> anchorUs(nbStreams, AV_NOPTS_VALUE);
  QVector<int> keptPerStream(nbStreams, 0);
  int keptPackets = 0;

  for (const Input &in : inputs) {
    QString msg;
    AVFormatContext *ctx = openInput(in, &msg);
    if (!ctx)
      return fail(msg);
    InputContext holder = wrapInput(ctx);
    const int64_t offsetUs =
        static_cast<int64_t>(in.offset * AV_TIME_BASE + 0.5);
    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(ctx, pkt) >= 0) {
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
          if ((pkt->flags & AV_PKT_FLAG_KEY) &&
              pkt->dts != AV_NOPTS_VALUE && t >= cutKey)
            cutFound = true;
          else {
            av_packet_unref(pkt);
            continue;
          }
        }
      } else if (!cutFound || t < cutKey) {
        av_packet_unref(pkt);
        continue;
      }
      if (anchorUs[si] == AV_NOPTS_VALUE)
        anchorUs[si] = dtsUs + offsetUs;
      AVPacket *opkt = av_packet_alloc();
      av_packet_ref(opkt, pkt);
      const int64_t dd = dtsUs + offsetUs - anchorUs[si];
      const AVRational litSrc{1, 1000000};
      const AVRational litDst{1, 1000};
      opkt->pts = av_rescale_q(ptsUs + offsetUs - anchorUs[si],
                               AV_TIME_BASE_Q, outTb);
      opkt->dts = av_rescale_q(dd, litSrc, litDst);
      opkt->duration = pkt->duration;
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
  }

  if (keptPackets == 0)
    return fail("window matched no packets");

  if (keptSeconds)
    *keptSeconds =
        (firstVideoT >= 0 && lastVideoT >= firstVideoT)
            ? lastVideoT - firstVideoT
            : 0.0;

  fprintf(stderr, "CLIPDBG kept=%d (v=%d a=%d) cut=%.3f cutKey=%.3f\n",
          keptPackets, keptPerStream.value(0), keptPerStream.value(1),
          cutSec, cutKey);

  if (av_write_trailer(outCtx) < 0)
    return fail("cannot write trailer");
  if (outCtx->pb)
    avio_flush(outCtx->pb);
  return true;
}

} // namespace TrophyBackend
