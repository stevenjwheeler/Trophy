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
 * Stream-copies packets (no re-encode): the head cut lands on the first
 * video keyframe at or after (newestEnd - windowSeconds), so the clip may
 * run up to one GOP longer than requested. Per-stream output timestamps
 * must be non-decreasing, so packets older than the high-water mark
 * (overlap between rotated segments) are dropped.
 */
class ReplayClipper {
public:
  static bool saveTail(const QList<ReplaySegment> &segments,
                       double windowSeconds, const QString &outputPath,
                       QString *error = nullptr);
};

} // namespace TrophyBackend
