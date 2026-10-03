/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "capture/IVideoCaptureSource.h"
#include "encoding/AudioEncoder.h"
#include "audio/AudioDriftCorrector.h"
#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include "FrameDispatcher.h"


namespace TrophyBackend {

class IVideoCaptureSource;
class IEncoder;
class IMuxer;
class AudioMixer;

/**
 * @brief Manages an active recording session
 *
 * Coordinates capture, encoding, and muxing threads for a single recording.
 */
class RecordingSession : public QObject {
  Q_OBJECT

public:
  RecordingSession(const QString &outputPath, IVideoCaptureSource *capture,
                   IEncoder *encoder, IMuxer *muxer, AudioMixer *audioMixer,
                   bool separateAudioTracks, int audioBitrate,
                   int activeAudioTrackCount,
                   const QMap<QString, QString> &metadata = {},
                   QObject *parent = nullptr);
  ~RecordingSession() override;

  bool start();
  // keepCaptureSource skips stopping the capture + callbacks, so a new
  // session can attach to the still-running stream (DVR rotation).
  void stop(bool keepCaptureSource = false);

  bool isRunning() const { return m_running; }

  qint64 frameCount() const { return m_frameCount; }
  qint64 bytesWritten() const { return m_bytesWritten; }

  // Audio identities to watch for (called by RecordingManager/ReplayManager
  // during setup). Game takes a PID plus a list of name hints (window
  // process, portal app-id, detector display name — Proton nodes expose
  // only the exe basename, e.g. "deadlock.exe", with no PID/binary).
  void setGameApps(const QString &processName) {
    m_gamePid = 0;
    m_gameHints = processName.isEmpty() ? QStringList() : QStringList(processName);
  }
  void setGameAudioHints(uint32_t pid, const QStringList &hints) {
    m_gamePid = pid;
    m_gameHints = hints;
  }
  void setVoiceChatApp(const QString &appName) { m_voiceChatApp = appName; }
  // Mixer track reserved for voice (-1 when merged mode left it unslotted;
  // the retry then skips voice instead of clobbering the mic's slot).
  void setVoiceTrack(int track) { m_voiceTrack = track; }
  // Track 0 is on monitor fallback (full mix), not the real app node — the
  // retry keeps hunting instead of treating it as connected.
  void setGameIsFallback(bool fallback) { m_gameIsFallback = fallback; }

  void setFrameDispatcher(std::shared_ptr<TrophyBackend::FrameDispatcher> d) { m_frameDispatcher = d; }
  bool pushQueuedFrame(QueuedFrame &&frame);

  // Mark a track as already having a source so the node-check timer won't re-add it
  void markSourceConnected(int trackIndex) {
    if (trackIndex >= 0 && trackIndex < kAudioTrackCount)
      m_sourceConnected[trackIndex] = true;
  }
  // Forget a track's bind (used when the user retargets mid-session: the
  // old source is removed and the retry hunts the new identity).
  void forgetTrack(int trackIndex) {
    if (trackIndex >= 0 && trackIndex < kAudioTrackCount) {
      m_sourceConnected[trackIndex] = false;
      m_boundSerial[trackIndex].clear();
    }
  }

signals:
  void errorOccurred(const QString &error);
  void frameEncoded(qint64 frameNumber);

private:
  void encodingLoop();
  void checkForNewAudioNodes();
  // Reconcile one app-capture track against the live registry: bind on
  // appearance, hot-swap when the stream generation changes, forget when
  // the node vanishes. isFallback (game track only) tracks monitor-fallback
  // state; it never counts as bound.
  void reconcileAppTrack(int track, uint32_t pid, const QStringList &hints,
                         bool *isFallback);

  // Track index to muxer stream index: audio stream N = track N + 1
  static constexpr int kAudioTrackCount = 3;

  QString m_outputPath;
  IVideoCaptureSource *m_capture;
  IEncoder *m_encoder;
  IMuxer *m_muxer;
  AudioMixer *m_audioMixer;

  // One encoder per audio track (game audio, voice chat, microphone)
  std::array<std::unique_ptr<AudioEncoder>, kAudioTrackCount> m_audioEncoders;
  std::array<std::mutex, kAudioTrackCount> m_audioEncoderMutexes;
  // Per-track wall-clock drift correctors (sound-card crystal vs video
  // clock). Invalid (SWR alloc failed) = pass-through.
  std::array<std::unique_ptr<AudioDriftCorrector>, kAudioTrackCount>
      m_driftCorrectors;

  // Merged audio track support
  bool m_separateAudioTracks = true;
  int m_audioBitrate = 192;
  int m_activeTrackCount = 0;
  std::unique_ptr<AudioEncoder> m_mergedEncoder;
  QMap<QString, QString> m_metadata;
  // Per-track accumulation buffers (merged mode) — different PipeWire sources
  // deliver different frame sizes, so each track accumulates independently.
  std::array<std::vector<float>, kAudioTrackCount> m_trackAccumulators;
  std::mutex m_mergeMutex;

  std::atomic<bool> m_running{false};
  std::atomic<qint64> m_frameCount{0};
  std::atomic<qint64> m_bytesWritten{0};
  // A/V origin alignment: audio and video pipelines start at different
  // wall times (portal video negotiates after audio flows) yet both
  // count timestamps from zero, baking in a constant offset. The first
  // video frame latches the video wall; audio fed before it is dropped
  // (unpairable head content) and late tracks get a one-time encoder
  // start offset. 10s video timeout so a dead portal can't mute audio.
  std::atomic<bool> m_videoStarted{false};
  std::atomic<int64_t> m_videoStartWallUs{-1};
  std::array<bool, kAudioTrackCount> m_trackOffsetApplied{};
  bool m_mergedOffsetApplied = false;
  static constexpr int64_t kVideoWaitTimeoutMs = 10000;
  static int64_t steadyNowUs();

  std::thread m_encodingThread;
  std::shared_ptr<TrophyBackend::FrameDispatcher> m_frameDispatcher;

  // Video frame queue for producer/consumer
  std::queue<QueuedFrame> m_videoQueue;
  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  const size_t MAX_QUEUE_SIZE = 30; // ~0.5s of buffer at 60fps

  // Audio node reconnection tracking
  QTimer *m_audioNodeCheckTimer = nullptr;
  uint32_t m_gamePid = 0;       // Game audio PID to watch for (0 = unknown)
  QStringList m_gameHints;      // Game name hints (exe, app-id, display name)
  QString m_voiceChatApp;       // Voice chat app name to watch for
  int m_voiceTrack = 1;         // Mixer slot reserved for voice (-1 = none)
  // Per-track bind state: bound serial of the node we're capturing (empty
  // when unbound). Track 0 additionally remembers monitor-fallback, which
  // never counts as bound — the retry keeps hunting the real app node.
  std::array<QString, kAudioTrackCount> m_boundSerial{};
  std::array<bool, kAudioTrackCount> m_sourceConnected{};
  // Session wall start (steady clock): video-wait timeout baseline.
  int64_t m_sessionStartMs = -1;
  bool m_gameIsFallback = false;
  std::array<int, kAudioTrackCount> m_trackToStreamMap{};  // Map track index to muxer stream index
};

} // namespace TrophyBackend
