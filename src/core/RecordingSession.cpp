/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "RecordingSession.h"
#include "audio/AudioMixer.h"
#include "audio/PipeWireAudioSource.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"

#include <QDebug>
#include <algorithm>

extern "C" {
#include <pipewire/pipewire.h>
}

namespace TrophyBackend {

RecordingSession::RecordingSession(const QString &outputPath,
                                   IVideoCaptureSource *capture,
                                   IEncoder *encoder, IMuxer *muxer,
                                   AudioMixer *audioMixer,
                                   bool separateAudioTracks, int audioBitrate,
                                   int activeAudioTrackCount, QObject *parent)
    : QObject(parent), m_outputPath(outputPath), m_capture(capture),
      m_encoder(encoder), m_muxer(muxer), m_audioMixer(audioMixer),
      m_separateAudioTracks(separateAudioTracks), m_audioBitrate(audioBitrate),
      m_activeTrackCount(activeAudioTrackCount) {
  m_trackToStreamMap.fill(-1);
}

RecordingSession::~RecordingSession() { stop(); }

bool RecordingSession::start() {
  if (m_running) {
    qWarning() << "Recording session already running";
    return false;
  }

  qDebug() << "Starting recording session to:" << m_outputPath;

  // Initialize state BEFORE starting any threads or callbacks
  m_frameCount = 0;
  m_bytesWritten = 0;
  m_running = true;
  m_paused = false;

  if (!m_muxer->initialize(m_outputPath)) {
    m_running = false;
    emit errorOccurred("Failed to initialize muxer");
    return false;
  }

  // --- Video stream (always stream 0) ---
  StreamInfo videoStream;
  videoStream.type = StreamInfo::Type::Video;
  videoStream.codecId = m_encoder->codecId();
  videoStream.width = m_encoder->width();
  videoStream.height = m_encoder->height();
  videoStream.fps = m_encoder->fps();
  videoStream.bitrate = m_encoder->bitrate();
  videoStream.preset = m_encoder->preset();
  videoStream.extraData = m_encoder->extraData();

  if (m_muxer->addStream(videoStream) < 0) {
    m_running = false;
    emit errorOccurred("Failed to add video stream to muxer");
    return false;
  }

  // --- Audio streams ---
  if (m_audioMixer) {
    AudioEncoderConfig audioConfig;
    audioConfig.sampleRate = m_audioMixer->sampleRate();
    audioConfig.channels = m_audioMixer->channels();
    audioConfig.bitrate = m_audioBitrate;
    audioConfig.codec = "aac";

    if (m_separateAudioTracks) {
      const QStringList trackNames = {"Game Audio", "Voice Chat", "Microphone"};
      qDebug() << "Separate tracks mode: initializing" << kAudioTrackCount
              << "audio tracks";

      for (int i = 0; i < kAudioTrackCount; ++i) {
        m_audioEncoders[i] = std::make_unique<AudioEncoder>();
        if (!m_audioEncoders[i]->initialize(audioConfig)) {
          qWarning() << "Failed to init audio encoder for track" << i
                     << "– skipping";
          m_audioEncoders[i].reset();
          continue;
        }
        StreamInfo s;
        s.type = StreamInfo::Type::Audio;
        s.codecId = m_audioEncoders[i]->codecId();
        s.sampleRate = audioConfig.sampleRate;
        s.channels = audioConfig.channels;
        s.bitrate = audioConfig.bitrate;
        s.extraData = m_audioEncoders[i]->extraData();
        int idx = m_muxer->addStream(s);
        if (idx >= 0) {
          qDebug() << "Added separate audio stream" << idx << "for"
                   << trackNames[i];
          m_trackToStreamMap[i] = idx;
        }
      }

      m_audioMixer->setTrackAudioCallback(
          [this](int trackIndex, const float *samples, int frameCount,
                 int sampleRate, int channels) {
            if (!m_running || m_paused)
              return;
            if (trackIndex < 0 || trackIndex >= kAudioTrackCount)
              return;
            if (!m_audioEncoders[trackIndex])
              return;

            EncodedPacket packet;
            std::lock_guard<std::mutex> lock(m_audioEncoderMutexes[trackIndex]);
            if (m_audioEncoders[trackIndex]->encodeAudio(samples, frameCount,
                                                         packet)) {
              int streamIdx = m_trackToStreamMap[trackIndex];
              if (streamIdx >= 0) {
                packet.streamIndex = streamIdx;
                if (m_muxer->writePacket(packet)) {
                    m_bytesWritten = m_muxer->bytesWritten();
                }
              }
            }
          });

    } else {
      m_mergedEncoder = std::make_unique<AudioEncoder>();
      if (!m_mergedEncoder->initialize(audioConfig)) {
        qWarning() << "Failed to init merged audio encoder";
        m_mergedEncoder.reset();
      } else {
        StreamInfo s;
        s.type = StreamInfo::Type::Audio;
        s.codecId = m_mergedEncoder->codecId();
        s.sampleRate = audioConfig.sampleRate;
        s.channels = audioConfig.channels;
        s.bitrate = audioConfig.bitrate;
        s.extraData = m_mergedEncoder->extraData();
        int idx = m_muxer->addStream(s);
        if (idx >= 0)
          qDebug() << "Added merged audio stream" << idx;
      }

      m_audioMixer->setTrackAudioCallback(
          [this](int trackIndex, const float *samples, int frameCount,
                 int sampleRate, int channels) {
            if (!m_running || m_paused || !m_mergedEncoder)
              return;
            if (trackIndex < 0 || trackIndex >= m_activeTrackCount)
              return;

            std::lock_guard<std::mutex> lock(m_mergeMutex);

            const int incoming = frameCount * channels;
            auto &buf = m_trackAccumulators[trackIndex];
            buf.insert(buf.end(), samples, samples + incoming);

            const int needed = 1024 * channels;
            while (true) {
              bool allReady = true;
              for (int i = 0; i < m_activeTrackCount; ++i) {
                if (static_cast<int>(m_trackAccumulators[i].size()) < needed) {
                  allReady = false;
                  break;
                }
              }
              if (!allReady)
                break;

              std::vector<float> mixed(needed, 0.0f);
              for (int i = 0; i < m_activeTrackCount; ++i) {
                for (int s = 0; s < needed; ++s)
                  mixed[s] += m_trackAccumulators[i][s];
                m_trackAccumulators[i].erase(m_trackAccumulators[i].begin(),
                                             m_trackAccumulators[i].begin() +
                                                 needed);
              }
              for (auto &v : mixed)
                v = std::clamp(v, -1.0f, 1.0f);

              EncodedPacket packet;
              if (m_mergedEncoder->encodeAudio(mixed.data(), 1024, packet)) {
                packet.streamIndex = 1;
                if (m_muxer->writePacket(packet)) {
                    m_bytesWritten = m_muxer->bytesWritten();
                }
              }
            }
          });
    }
  }

  // Write container header
  if (!m_muxer->writeHeader()) {
    m_running = false;
    emit errorOccurred("Failed to write muxer header");
    return false;
  }

  // Set video capture callback
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

  if (!m_capture->startCapture()) {
    m_running = false;
    emit errorOccurred("Failed to start video capture");
    return false;
  }

  if (m_audioMixer) {
    m_audioMixer->startAll();
  }

  if (!m_gameProcess.isEmpty() || !m_voiceChatApp.isEmpty()) {
    m_audioNodeCheckTimer = new QTimer(this);
    m_audioNodeCheckTimer->setInterval(3000);
    connect(m_audioNodeCheckTimer, &QTimer::timeout, this,
            &RecordingSession::checkForNewAudioNodes);
    m_audioNodeCheckTimer->start();
    checkForNewAudioNodes(); 
  }

  m_encodingThread = std::thread([this]() { encodingLoop(); });

  return true;
}

void RecordingSession::stop() {
  if (!m_running) {
    return;
  }

  qDebug() << "Stopping recording session...";

  if (m_audioNodeCheckTimer) {
    m_audioNodeCheckTimer->stop();
    m_audioNodeCheckTimer->deleteLater();
    m_audioNodeCheckTimer = nullptr;
  }

  if (m_capture) {
    m_capture->stopCapture();
    m_capture->setFrameCallback(nullptr);
  }

  if (m_audioMixer) {
    m_audioMixer->stopAll();
    m_audioMixer->setTrackAudioCallback(nullptr);
  }

  m_running = false;
  m_queueCv.notify_all();

  if (m_encodingThread.joinable()) {
    m_encodingThread.join();
  }

  for (int i = 0; i < kAudioTrackCount; ++i) {
    if (!m_audioEncoders[i])
      continue;

    std::vector<EncodedPacket> flushed;
    m_audioEncoders[i]->flush(flushed);
    for (auto &pkt : flushed) {
      if (m_trackToStreamMap[i] >= 0) {
        pkt.streamIndex = m_trackToStreamMap[i];
        m_muxer->writePacket(pkt);
      }
    }
  }

  if (m_mergedEncoder) {
    std::vector<EncodedPacket> flushed;
    m_mergedEncoder->flush(flushed);
    for (auto &pkt : flushed) {
      pkt.streamIndex = 1;
      m_muxer->writePacket(pkt);
    }
  }

  if (m_muxer) {
    m_muxer->finalize();
    
    m_bytesWritten = m_muxer->bytesWritten();
  }

  qDebug() << "Recording session stopped. Frames:" << m_frameCount
          << "Size:" << m_bytesWritten << "bytes";
}

void RecordingSession::pause() { m_paused = true; }

void RecordingSession::resume() { m_paused = false; }

void RecordingSession::encodingLoop() {
  qDebug() << "Encoding thread started";

  while (true) {
    QueuedFrame frame(VideoFrame{nullptr, 0, 0, 0, 0, 0});
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      m_queueCv.wait(lock,
                     [this] { return !m_videoQueue.empty() || !m_running; });

      if (m_videoQueue.empty() && !m_running)
        break;

      frame = std::move(m_videoQueue.front());
      m_videoQueue.pop();
    }

    EncodedPacket packet;
    if (m_encoder->encodeFrame(frame, packet)) {
      packet.streamIndex = 0;
      if (m_muxer->writePacket(packet)) {
        m_bytesWritten = m_muxer->bytesWritten();
      }
      m_frameCount++;

      if (m_frameCount % 60 == 0) {
        emit frameEncoded(m_frameCount);
      }
    }
  }

  std::vector<EncodedPacket> flushedPackets;
  if (m_encoder && m_encoder->flush(flushedPackets) > 0) {
    for (auto &pkt : flushedPackets) {
      pkt.streamIndex = 0;
      m_muxer->writePacket(pkt);
    }
  }

  qDebug() << "Encoding thread stopped";
}

void RecordingSession::checkForNewAudioNodes() {
  if (!m_running || !m_audioMixer) {
    return;
  }

  bool allConnected = true;

  if (!m_sourceConnected[0] && !m_gameProcess.isEmpty()) {
    allConnected = false;
    uint32_t nodeId = PW_ID_ANY;
    QString nodeName, nodeSerial;

    nodeId = PipeWireAudioSource::findNodeByProcessName(
        m_gameProcess, &nodeName, &nodeSerial);

    if (nodeId != PW_ID_ANY) {
      qDebug() << "Game audio node appeared, connecting:"
              << (nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
      m_audioMixer->addSource(PipeWireAudioSource::createApplicationSource(
                                  nodeId, nodeName, nodeSerial),
                              0);
      m_sourceConnected[0] = true;
    }
  }

  if (!m_sourceConnected[1] && !m_voiceChatApp.isEmpty()) {
    allConnected = false;
    QString nodeName, nodeSerial;
    uint32_t nodeId = PipeWireAudioSource::findNodeByProcessName(
        m_voiceChatApp, &nodeName, &nodeSerial);

    if (nodeId != PW_ID_ANY) {
      qDebug() << "Voice chat app node appeared, connecting:"
              << (nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
      m_audioMixer->addSource(PipeWireAudioSource::createApplicationSource(
                                  nodeId, nodeName, nodeSerial),
                              1);
      m_sourceConnected[1] = true;
    }
  }

  if (allConnected && m_audioNodeCheckTimer) {
    m_audioNodeCheckTimer->stop();
    qDebug() << "All expected audio nodes connected, stopping node checks";
  }
}

} // namespace TrophyBackend