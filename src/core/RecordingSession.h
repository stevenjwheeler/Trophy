/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "capture/IVideoCaptureSource.h"
#include "encoding/AudioEncoder.h"
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

namespace GameRecorder {

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
                   QObject *parent = nullptr);
  ~RecordingSession() override;

  bool start();
  void stop();
  void pause();
  void resume();

  bool isRunning() const { return m_running; }
  bool isPaused() const { return m_paused; }

  qint64 frameCount() const { return m_frameCount; }
  qint64 bytesWritten() const { return m_bytesWritten; }

signals:
  void errorOccurred(const QString &error);
  void frameEncoded(qint64 frameNumber);

private:
  void encodingLoop();
  void audioLoop();

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

  // Merged audio track support
  bool m_separateAudioTracks = true;
  int m_audioBitrate = 192;
  int m_activeTrackCount = 0;
  std::unique_ptr<AudioEncoder> m_mergedEncoder;
  std::vector<float> m_mixAccumulator;
  int m_tracksContributed = 0;  // plain int, always under m_mergeMutex
  std::mutex m_mergeMutex;

  std::atomic<bool> m_running{false};
  std::atomic<bool> m_paused{false};
  std::atomic<qint64> m_frameCount{0};
  std::atomic<qint64> m_bytesWritten{0};

  std::thread m_encodingThread;

  // Video frame queue for producer/consumer
  std::queue<QueuedFrame> m_videoQueue;
  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  const size_t MAX_QUEUE_SIZE = 30; // ~0.5s of buffer at 60fps
};

} // namespace GameRecorder
