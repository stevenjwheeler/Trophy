/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IMuxer.h"
#include <QMap>
#include <mutex>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace TrophyBackend {

class FFmpegMuxer : public IMuxer {
  Q_OBJECT

public:
  explicit FFmpegMuxer(QObject *parent = nullptr);
  ~FFmpegMuxer() override;

  bool initialize(const QString &outputPath) override;
  int addStream(const StreamInfo &info) override;
  bool writeHeader() override;
  bool setMetadata(const QMap<QString, QString> &metadata) override;
  bool writePacket(const EncodedPacket &packet) override;
  bool finalize() override;

  bool updateStreamExtraData(int streamIndex, const std::vector<uint8_t> &data) override;

  QString outputPath() const override { return m_outputPath; }
  int64_t bytesWritten() const override;
  QString formatName() const override;

private:
  void cleanup();
  void cleanup_internal(); // Added this declaration

  QString m_outputPath;
  AVFormatContext *m_formatCtx = nullptr;
  std::vector<AVStream *> m_streams;
  
  bool m_headerWritten = false;
  int64_t m_globalOffset = -1;
  
  mutable std::mutex m_muxerMutex; 
};

} // namespace TrophyBackend