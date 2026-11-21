/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"
#include <QJsonObject>
#include <QVariantMap>
#include <memory>

namespace TrophyBackend {

struct EncodedStream {
  quint64 generation = 0;
  qint64 timelineStartMs = 0;
  QString game;
  QVariantMap identity;
  QMap<QString, QString> metadata;
  std::vector<StreamInfo> streams;
};

using EncodedFrame = std::shared_ptr<const EncodedPacket>;

// Output implementations receive immutable encoded data, never capture sources,
// encoders, audio mixers, sessions, restore tokens, or pipeline-control callbacks.
// The engine serializes begin/write/end for each output away from the GUI thread.
class EncodedOutput {
public:
  virtual ~EncodedOutput() = default;
  virtual bool begin(const EncodedStream &stream) = 0;
  virtual bool write(const EncodedFrame &packet) = 0;
  virtual bool end() = 0;
  virtual bool started() const = 0;
  virtual QString error() const = 0;
  virtual void setAudioHistory(quint64 generation, const QJsonObject &history) = 0;
};
} // namespace TrophyBackend
