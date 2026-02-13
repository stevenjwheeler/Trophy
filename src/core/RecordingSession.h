/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "capture/IVideoCaptureSource.h"
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
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

  QString m_outputPath;
  IVideoCaptureSource *m_capture;
  IEncoder *m_encoder;
  IMuxer *m_muxer;
  AudioMixer *m_audioMixer;

  std::atomic<bool> m_running{false};
  std::atomic<bool> m_paused{false};
  std::atomic<qint64> m_frameCount{0};
  std::atomic<qint64> m_bytesWritten{0};

  std::thread m_encodingThread;
  std::thread m_audioThread;

  // Video frame queue for producer/consumer
  std::queue<QueuedFrame> m_videoQueue;
  std::mutex m_queueMutex;
  std::condition_variable m_queueCv;
  const size_t MAX_QUEUE_SIZE = 30; // ~0.5s of buffer at 60fps
};

} // namespace GameRecorder
