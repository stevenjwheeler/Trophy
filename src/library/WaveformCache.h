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

  // Number of audio streams (0 when the file won't open).
  // Synchronous, but only opens the container (no decode).
  Q_INVOKABLE int audioTrackCount(const QString &filePath);

  // Async peaks for one audio stream (0 = first). Emits
  // peaksReady even on failure (with an empty list) so QML
  // never waits forever.
  Q_INVOKABLE void requestPeaks(const QString &filePath, int track);

  // Lane label: "Audio" for single-track files, else
  // Game/Voice/Mic matching the recorder's track order.
  Q_INVOKABLE static QString trackLabel(int track, int trackCount);

  static constexpr int kBuckets = 8000;

signals:
  void peaksReady(const QString &filePath, int track,
                  const QVariantList &peaks);

private:
  // In-flight files and the tracks still owed an emit.
  QMap<QString, QSet<int>> m_pending;
};

} // namespace TrophyBackend
