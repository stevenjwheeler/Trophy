/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IMuxer.h"
#include <mutex>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace TrophyBackend {

/**
 * @brief FFmpeg-based container muxer
 *
 * Supports MKV and MP4 output formats.
 */
class FFmpegMuxer : public IMuxer {
  Q_OBJECT

public:
  explicit FFmpegMuxer(QObject *parent = nullptr);
  ~FFmpegMuxer() override;

  bool initialize(const QString &outputPath) override;
  int addStream(const StreamInfo &info) override;
  bool writeHeader() override;
  bool writePacket(const EncodedPacket &packet) override;
  bool finalize() override;

  QString outputPath() const override { return m_outputPath; }
  int64_t bytesWritten() const override { return m_bytesWritten; }
  QString formatName() const override;

private:
  void cleanup();

  QString m_outputPath;
  AVFormatContext *m_formatCtx = nullptr;
  std::vector<AVStream *> m_streams;
  std::mutex m_writeMutex;
  int64_t m_bytesWritten = 0;
  bool m_headerWritten = false;
  int64_t m_globalOffset = -1;
};

} // namespace TrophyBackend