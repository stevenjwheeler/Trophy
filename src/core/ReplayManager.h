/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <cstdint>
#include <memory>

#include "muxing/ReplayClipper.h"

namespace TrophyBackend {

class SettingsManager;
class AudioMixer;
class IVideoCaptureSource;
class IEncoder;
class IMuxer;
class RecordingSession;

/**
 * @brief Instant-replay DVR: rolling 60s segments while a game runs.
 *
 * While active, a full capture→encode→mux pipeline writes segments into
 * a cache dir (configurable, e.g. /tmp for RAM-backed storage), keeping
 * enough to cover the clip length plus one segment of margin. saveReplay
 * finalizes the current segment and stream-copies the trailing window
 * into the library — no re-encode, ready in seconds.
 *
 * Paused (and cleared) during real recordings; the DVR owns a dedicated
 * mixer so stopping it can't tear down the recording's audio sources.
 */
class ReplayManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool active READ isActive NOTIFY replayStateChanged)
  Q_PROPERTY(QString currentGame READ currentGame NOTIFY replayStateChanged)

public:
  static constexpr int kSegmentSeconds = 60;
  static constexpr int kPruneMarginSeconds = 60;
  static constexpr int kMinRetainedSegments = 2;

  explicit ReplayManager(QObject *parent = nullptr);
  ~ReplayManager() override;

  void setSettingsManager(SettingsManager *settings);

  bool isActive() const { return m_state == State::Running; }
  QString currentGame() const { return m_gameName; }

  // Pure helpers (unit-tested): window/retention over segment lists.
  struct Segment {
    QString path;
    double startSec = 0.0; // since DVR start
    double durationSec = 0.0;
    double endSec() const { return startSec + durationSec; }
  };
  static QList<ReplaySegment> selectForWindow(const QList<Segment> &segments,
                                              double nowSec, double windowSec);
  static QStringList pruneList(const QList<Segment> &segments, double nowSec,
                               double windowSec);

public slots:
  void startDvr(const QString &gameName);
  void stopDvr();
  // Paused during real recordings: stops the loop and clears the buffer.
  void setPaused(bool paused);
  void saveReplay();

signals:
  void replayStateChanged();
  void replaySaved(const QString &filePath, double seconds);
  void replayFailed(const QString &reason);
  /**
   * The portal rejected our saved restore token. The app deletes it and
   * re-authorizes through the New Game prompt — never a bare picker.
   */
  void restoreAuthNeeded(const QString &gameName);

private slots:
  void onCaptureReady();
  void onCaptureError(const QString &error);
  void rotateSegment();

private:
  enum class State { Idle, StartingCapture, Running };

  void setState(State s);
  bool beginCapture();
  bool beginSegment();
  void finalizeSegment(); // stop session, keep capture running
  void pruneOldSegments(double now);
  void clearSegments();
  QString cacheDir() const;
  // The pre-/tmp default (CacheLocation + "/trophy/replay"). Never written
  // anymore; cacheDir() removes it once so upgraders don't keep the stale
  // dir (it used to sit next to Qt's own caches as Trophy/trophy/replay).
  static QString legacyCacheDir();
  QString clipOutputPath() const;
  double elapsedSec() const;
  void setupAudioSources(int &trackCount, QStringList &gameHints,
                           uint32_t &gamePid, bool &gameFound, int &voiceTrack,
                           bool &voiceFound, int &micTrack);

  State m_state = State::Idle;
  QString m_gameName;
  bool m_paused = false;
  bool m_saving = false;

 public:
  // Live audio retargeting, mirroring RecordingManager (acts on the open
  // segment's session; no-ops when the DVR is idle — the next segment
  // picks the new setting up).
  void applyMicrophoneDevice();
  void applyGameAudioDevice();
  void applyVoiceChatApp();

  // Mixer passthroughs for the Application-level mute proxy.
  void setTrackMuted(int trackIndex, bool muted);
  bool isTrackMuted(int trackIndex) const;
  float trackLevel(int trackIndex) const;

 private:
  // Mixer slots assigned per segment (needed to retarget mid-segment).
  int m_micTrack = -1;
  int m_voiceTrack = -1;

  // Test seam (same pattern as GameDetectionServiceTest).
  friend class ReplayManagerTest;

  SettingsManager *m_settings = nullptr;
  // Dedicated mixer: sharing RecordingManager's mixer caused teardown
  // races (DVR stop ripped out the recording's just-added sources).
  // PipeWire happily serves both while they briefly overlap.
  std::unique_ptr<AudioMixer> m_audioMixer;

  std::unique_ptr<IVideoCaptureSource> m_capture;
  std::unique_ptr<IEncoder> m_encoder;
  std::unique_ptr<IMuxer> m_muxer;
  std::unique_ptr<RecordingSession> m_session;

  QList<Segment> m_segments;
  QElapsedTimer m_dvrClock;
  QTimer *m_rotationTimer = nullptr;
  int m_segmentIndex = 0;
  double m_openSegmentStart = 0.0;
  QString m_openSegmentPath;
};

} // namespace TrophyBackend
