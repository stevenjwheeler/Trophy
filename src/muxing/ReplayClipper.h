/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
}

namespace TrophyBackend {

namespace ReplayClipperInternal {
// Production defaults are actual FFmpeg functions. Overrides are private,
// thread-local and scoped by the test friend (never runtime environment flags).
struct Operations {
  decltype(&avformat_open_input) openInput = &avformat_open_input;
  decltype(&avformat_find_stream_info) probeInput = &avformat_find_stream_info;
  decltype(&avio_open) openOutput = &avio_open;
  decltype(&av_read_frame) readFrame = &av_read_frame;
  decltype(&av_packet_alloc) packetAlloc = &av_packet_alloc;
  decltype(&av_packet_ref) packetRef = &av_packet_ref;
  decltype(&avformat_alloc_output_context2) allocOutput =
      &avformat_alloc_output_context2;
  decltype(&avformat_new_stream) newStream = &avformat_new_stream;
  decltype(&avcodec_parameters_copy) copyParameters = &avcodec_parameters_copy;
  decltype(&av_dict_copy) copyMetadata = &av_dict_copy;
  decltype(&av_dict_set) setMetadata = &av_dict_set;
  decltype(&avformat_write_header) writeHeader = &avformat_write_header;
  decltype(&av_interleaved_write_frame) writeFrame = &av_interleaved_write_frame;
  decltype(&av_write_trailer) writeTrailer = &av_write_trailer;
  decltype(&avio_flush) flush = &avio_flush;
  decltype(&avio_closep) closeOutput = &avio_closep;
};
} // namespace ReplayClipperInternal

/**
 * @brief A DVR segment: a self-contained recording file plus its start
 * offset in seconds relative to the first segment.
 */
struct ReplaySegment {
  QString path;
  double startOffsetSec = 0.0;
};

/**
 * @brief Cuts the trailing window of DVR segments into a single clip.
 *
 * Stream-copies packets (no re-encode): the head cut lands on the last
 * video keyframe at or before (newestEnd - windowSeconds) — first one
 * after it when the buffer is shorter than the window — so the clip
 * overshoots the window by up to ~one GOP (two when the cut lands in a
 * segment's first GOP, whose head keyframe lacks a demuxable DTS), never
 * undershoots it. Per-stream output timestamps must be non-decreasing,
 * so packets older than the high-water mark (overlap between rotated
 * segments) are dropped. Later segment heads with missing DTS are recovered
 * from bounded lookahead and known decode durations; if a reference keyframe
 * cannot be retained, video waits for the next safe GOP rather than copying
 * dependent pictures without their references; that fallback may leave a gap.
 */
class ReplayClipper {
  friend struct ReplayClipperTestAccess;
  static thread_local const ReplayClipperInternal::Operations *s_operations;

public:
  static bool saveTail(const QList<ReplaySegment> &segments,
                       double windowSeconds, const QString &outputPath,
                       QString *error = nullptr,
                       double *keptSeconds = nullptr);

  /**
   * @brief Cuts an arbitrary [startSeconds, endSeconds] range out of a
   * single library file into a clip.
   *
   * Same engine as saveTail (single input at offset 0): the head cut lands
   * on the last video keyframe at or before startSeconds (first one after
   * it when the range starts before the first keyframe), so the clip
   * overshoots the requested start by up to ~one GOP, never undershoots
   * it. Packets past endSeconds are dropped on every stream. Container
   * tags are carried and re-stamped exactly like saveTail (trophy-kind
   * clip, own save moment), so player-made clips badge and date like
   * DVR clips.
   */
  static bool saveRange(const QString &sourcePath, double startSeconds,
                        double endSeconds, const QString &outputPath,
                        QString *error = nullptr,
                        double *keptSeconds = nullptr);
};

} // namespace TrophyBackend
