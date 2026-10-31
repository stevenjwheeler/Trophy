/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFutureWatcher>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

namespace TrophyBackend {

/** One PipeWire playback stream attributed to a process. */
struct AudioStreamInfo {
  quint32 nodeId = 0;
  qint64 pid = 0;
  QString binary;
  QString name;

  bool operator==(const AudioStreamInfo &other) const = default;
};

/**
 * @brief Desktop-agnostic launch/PID signal from PipeWire playback streams.
 *
 * Every audio-producing application exposes `application.process.id` and
 * `application.process.binary` on its PipeWire node, independent of the
 * compositor. Watching that list gives detection a cross-DE PID source
 * without a preload library or a per-DE window backend.
 *
 * Snapshots are produced off the GUI thread (the existing audio enumerator
 * does a bounded PipeWire round-trip) and diffed here. A stream that changes
 * PID/binary/name is re-announced as `streamAppeared` so consumers can
 * re-evaluate identity; `streamRemoved` means the node disappeared.
 */
class PipeWireStreamWatcher : public QObject {
  Q_OBJECT

public:
  using SnapshotFn = std::function<QVariantList()>;

  explicit PipeWireStreamWatcher(QObject *parent = nullptr);
  ~PipeWireStreamWatcher() override;

  /**
   * @brief Snapshot provider (usually PipeWireAudioSource::enumerateAudioApps).
   * Runs on a worker thread; it must be a plain function that does not touch
   * this object or GUI state.
   */
  void setSnapshotFunction(SnapshotFn fn);

  void start(int intervalMs = 1000);
  void stop();
  bool isRunning() const;
  QList<AudioStreamInfo> streams() const;

  Q_INVOKABLE void pollNow();

signals:
  void streamAppeared(const TrophyBackend::AudioStreamInfo &stream);
  void streamRemoved(quint32 nodeId);
  void streamsChanged();

private:
  void poll();
  void applySnapshot(const QVariantList &apps);

  SnapshotFn m_snapshot;
  QTimer m_timer;
  QFutureWatcher<QVariantList> m_watcher;
  QMap<quint32, AudioStreamInfo> m_known;
  bool m_inflight = false;
};

} // namespace TrophyBackend

Q_DECLARE_METATYPE(TrophyBackend::AudioStreamInfo)
