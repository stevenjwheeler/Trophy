/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ReplayClipper.h"
namespace TrophyBackend {
// Worker-only output policy: incompatible video is normalized at export,
// never by constraining the capture to an earlier launcher's resolution.
class ReplayExporter {
public:
  static bool saveTail(const QList<ReplaySegment> &segments, double seconds,
                       const QString &path, QString *error, double *keptSeconds,
                       double endSeconds = std::numeric_limits<double>::infinity());
};
}
