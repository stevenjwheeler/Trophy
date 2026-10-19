/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QString>
#include <QList>
#include <QVariantMap>
#include <atomic>

namespace TrophyBackend {
struct ShareExportOptions {
  int maxHeight = 1080; // 0 = original, never upscale
  qint64 targetBytes = 0; // approximate per-file bitrate budget, 0 = quality mode
  QList<int> audioStreams; // absolute stream indices; empty = silent export
  bool watermark = true; // burned into share copies only
};
struct ShareExportResult { QString path; QString error; };
// Worker-only native FFmpeg operations. No ffmpeg executable dependency,
// hardware encoder discovery, or changes to the source recording.
class ShareExportEngine {
public:
  static QVariantMap inspect(const QString &source, std::atomic_bool &cancel);
  static ShareExportResult run(const QString &source, const QString &directory,
      const ShareExportOptions &options, std::atomic_bool &cancel, std::atomic<double> &progress);
};
}
