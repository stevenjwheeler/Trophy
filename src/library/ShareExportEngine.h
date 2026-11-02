/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QString>
#include <QList>
#include <QVariantMap>
#include <atomic>

namespace TrophyBackend {

// A share copy is always a container remux: the video bitstream is copied
// unchanged and the selected audio tracks are mixed down to one stereo track
// (MP3 in MP4; a lone stereo AAC track is copied instead of re-encoded).
// There is no video re-encode path (no resolution, size target or
// watermark), so exports are limited by disk I/O rather than encoding.
struct ShareExportOptions {
  QList<int> audioStreams; // absolute stream indices to mix; empty = silent
  enum class Mode {
    Copy,    // remux: video bitstream copied, audio mixed/encoded (fast, lossless)
    Convert  // re-encode to H.264 + AAC for sources MP4 sharing cannot use as-is
  };
  Mode mode = Mode::Copy;
  int maxHeight = 0; // Convert only: 0 keeps the source size, otherwise 720/1080
};

struct ShareExportResult { QString path; QString error; };

// Worker-only native FFmpeg operations. No ffmpeg executable dependency,
// hardware encoder discovery, or changes to the source recording.
class ShareExportEngine {
public:
  // Reports duration, per-track labels and whether the video codec can be
  // placed in MP4 (`copyable`) / is the universally shareable H.264
  // (`shareable`).
  static QVariantMap inspect(const QString &source, std::atomic_bool &cancel);
  static ShareExportResult run(const QString &source, const QString &directory,
      const ShareExportOptions &options, std::atomic_bool &cancel, std::atomic<double> &progress);
};
}
