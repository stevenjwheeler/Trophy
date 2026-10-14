/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <functional>

namespace TrophyBackend {

/**
 * @brief Per-track waveform peaks for the in-app player
 *
 * Decodes an audio stream to mono float, buckets the peak
 * amplitudes and hands them to QML for drawing. Work runs on
 * the global thread pool (a 2h game track decodes in seconds,
 * but never on the GUI thread); results arrive via
 * peaksReady. Computed peaks are cached next to the
 * thumbnails (`.thumbnails/<base>.peaks`, validated against
 * file size + mtime) so reopening a file is instant.
 *
 * Buckets are fixed (8000/track): the lane downsamples when
 * drawing, so the cache never depends on window width — and
 * zoomed views still resolve real detail instead of chunky
 * bars (~2px/bucket at 16x on an hour file).
 */
class WaveformCache : public QObject {
  Q_OBJECT

public:
  explicit WaveformCache(QObject *parent = nullptr);
  ~WaveformCache() override = default;

  // Container probe for worker/test use: -1 means unavailable, 0 no audio.
  static int audioTrackCount(const QString &filePath);
  // Player probes must never open a container on the GUI thread. Caller
  // supplies an operation id so close/reopen of the same path is safe.
  Q_INVOKABLE void requestAudioTrackCount(const QString &filePath,
                                          int requestId);

  // Async peaks for one audio stream (0 = first). Emits
  // peaksReady even on failure (with an empty list) so QML
  // never waits forever.
  Q_INVOKABLE void requestPeaks(const QString &filePath, int track);

  // Lane label: "Audio" for single-track files, else
  // Game/Voice/Mic matching the recorder's track order.
  Q_INVOKABLE static QString trackLabel(int track, int trackCount);

  static constexpr int kBuckets = 8000;

signals:
  void audioTrackCountReady(const QString &filePath, int requestId, int count);
  void peaksReady(const QString &filePath, int track,
                  const QVariantList &peaks);

private:
  friend class WaveformCacheTestPeer;
  // Worker-local instrumentation, copied before scheduling. Empty in production.
  struct DecodeStats {
    int passes = 0;
    qint64 reads = 0;
    qint64 packets = 0;
    int tracks = 0;
    int decoderDrains = 0;
    int resamplerDrains = 0;
    qint64 samples = 0;
    qint64 drainSamples = 0;
    qint64 blocks = 0;
    qint64 blockCapacity = 0;
    qint64 outputCapacity = 0;
    qint64 maxOutputCapacity = 0;
    bool cleanEof = false;
  };
  std::function<void(const DecodeStats &)> m_decodeObserver;
  std::function<int(int, qint64)> m_readResultOverride;
  // In-flight files and the tracks still owed an emit.
  QMap<QString, QSet<int>> m_pending;
  // Exists only while delivering a settled job, so reentrant requests reuse
  // its memory even if the sidecar couldn't be written.
  QMap<QString, QVector<QVector<float>>> m_delivering;
};

} // namespace TrophyBackend
