/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QString>

namespace TrophyBackend {

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
 * segments) are dropped.
 */
class ReplayClipper {
public:
  static bool saveTail(const QList<ReplaySegment> &segments,
                       double windowSeconds, const QString &outputPath,
                       QString *error = nullptr,
                       double *keptSeconds = nullptr);
};

} // namespace TrophyBackend
