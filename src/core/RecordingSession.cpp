/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingSession.h"
#include "audio/AudioMixer.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"

#include <QDebug>

namespace GameRecorder {

RecordingSession::RecordingSession(const QString &outputPath,
                                   IVideoCaptureSource *capture,
                                   IEncoder *encoder, IMuxer *muxer,
                                   AudioMixer *audioMixer, QObject *parent)
    : QObject(parent), m_outputPath(outputPath), m_capture(capture),
      m_encoder(encoder), m_muxer(muxer), m_audioMixer(audioMixer) {}

RecordingSession::~RecordingSession() { stop(); }

bool RecordingSession::start() {
  if (m_running) {
    qWarning() << "Recording session already running";
    return false;
  }

  qInfo() << "Starting recording session to:" << m_outputPath;

  // Initialize muxer with output file
  if (!m_muxer->initialize(m_outputPath)) {
    emit errorOccurred("Failed to initialize muxer");
    return false;
  }

  // Add video stream to muxer
  StreamInfo videoStream;
  videoStream.type = StreamInfo::Type::Video;
  videoStream.codecId = m_encoder->codecId();
  videoStream.width = m_capture->nativeResolution().width();
  videoStream.height = m_capture->nativeResolution().height();
  videoStream.fps = 60; // TODO: Get from config
  videoStream.extraData = m_encoder->extraData();

  if (m_muxer->addStream(videoStream) < 0) {
    emit errorOccurred("Failed to add video stream to muxer");
    return false;
  }

  // Write container header
  if (!m_muxer->writeHeader()) {
    emit errorOccurred("Failed to write muxer header");
    return false;
  }

  m_running = true;
  m_paused = false;
  m_frameCount = 0;
  m_bytesWritten = 0;

  // Set capture callback to push frames into our queue
  m_capture->setFrameCallback([this](const VideoFrame &frame) {
    if (!m_running || m_paused)
      return;

    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_videoQueue.size() < MAX_QUEUE_SIZE) {
      m_videoQueue.emplace(frame);
      m_queueCv.notify_one();
    } else {
      qWarning() << "Video queue full, dropping frame";
    }
  });

  // Start capture
  if (!m_capture->startCapture()) {
    emit errorOccurred("Failed to start video capture");
    return false;
  }

  // Start encoding thread
  m_encodingThread = std::thread([this]() { encodingLoop(); });

  // Start audio thread
  m_audioThread = std::thread([this]() { audioLoop(); });

  return true;
}

void RecordingSession::stop() {
  if (!m_running) {
    return;
  }

  qInfo() << "Stopping recording session...";

  // Stop capture first so no new frames enter the queue
  if (m_capture) {
    m_capture->stopCapture();
    m_capture->setFrameCallback(nullptr);
  }

  m_running = false;
  m_queueCv.notify_all(); // Wake up encoding thread to finish

  // Wait for threads to finish
  if (m_encodingThread.joinable()) {
    m_encodingThread.join();
  }
  if (m_audioThread.joinable()) {
    m_audioThread.join();
  }

  // Finalize muxer
  if (m_muxer) {
    m_muxer->finalize();
  }

  qInfo() << "Recording session stopped. Frames:" << m_frameCount
          << "Bytes:" << m_bytesWritten;
}

void RecordingSession::pause() { m_paused = true; }

void RecordingSession::resume() { m_paused = false; }

void RecordingSession::encodingLoop() {
  qInfo() << "Encoding thread started";

  while (true) {
    QueuedFrame frame(VideoFrame{nullptr, 0, 0, 0, 0, 0});
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCv.wait(lock,
                     [this] { return !m_videoQueue.empty() || !m_running; });

      if (m_videoQueue.empty() && !m_running) {
        break;
      }

      frame = std::move(m_videoQueue.front());
      m_videoQueue.pop();
    }

    EncodedPacket packet;
    if (m_encoder->encodeFrame(frame, packet)) {
      packet.streamIndex = 0; // Video stream
      if (m_muxer->writePacket(packet)) {
        m_bytesWritten = m_muxer->bytesWritten();
      }
      m_frameCount++;

      if (m_frameCount % 60 == 0) {
        emit frameEncoded(m_frameCount);
      }
    }
  }

  // Flush encoder
  std::vector<EncodedPacket> flushedPackets;
  if (m_encoder && m_encoder->flush(flushedPackets) > 0) {
    for (auto &packet : flushedPackets) {
      packet.streamIndex = 0;
      m_muxer->writePacket(packet);
    }
  }

  qInfo() << "Encoding thread stopped";
}

void RecordingSession::audioLoop() {
  qInfo() << "Audio thread started";

  while (m_running) {
    if (m_paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    // TODO: Get audio from mixer
    // TODO: Encode audio
    // TODO: Write to muxer

    // Placeholder: process audio at ~20ms intervals
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  qInfo() << "Audio thread stopped";
}

} // namespace GameRecorder
