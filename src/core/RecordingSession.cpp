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
#include <chrono>

extern "C" {
#include <pipewire/pipewire.h>
}

namespace TrophyBackend {

RecordingSession::RecordingSession(const QString &outputPath,
                                   IVideoCaptureSource *capture,
                                   IEncoder *encoder, IMuxer *muxer,
                                   AudioMixer *audioMixer,
                                   bool separateAudioTracks, int audioBitrate,
                                   int activeAudioTrackCount,
                                   const QMap<QString, QString> &metadata,
                                   QObject *parent)
    : QObject(parent), m_outputPath(outputPath), m_capture(capture),
      m_encoder(encoder), m_muxer(muxer), m_audioMixer(audioMixer),
      m_separateAudioTracks(separateAudioTracks), m_audioBitrate(audioBitrate),
      m_activeTrackCount(activeAudioTrackCount), m_metadata(metadata) {
  m_trackToStreamMap.fill(-1);
}

RecordingSession::~RecordingSession() { stop(); }

int64_t RecordingSession::steadyNowUs() {
  // Milliseconds; matches m_sessionStartMs units (×1000 at use sites
  // needing microseconds).
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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
  // First-bind gate baseline (game track): nodes sighted with the
  // session bind at once; later arrivals must prove stability first.
  m_sessionStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  m_videoStarted = false;
  m_videoStartWallUs = -1;
  m_trackOffsetApplied.fill(false);
  m_mergedOffsetApplied = false;

  if (!m_muxer->initialize(m_outputPath)) {
    m_running = false;
    emit errorOccurred("Failed to initialize muxer");
    return false;
  }

  if (!m_muxer->setMetadata(m_metadata)) {
    m_running = false;
    emit errorOccurred("Failed to set muxer metadata");
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
  videoStream.pixelFormat = m_encoder->encodedPixelFormat();
  videoStream.extraData = m_encoder->extraData();

  if (videoStream.extraData.empty()) {
    qWarning() << "RecordingSession: encoder provided no extradata (SPS/PPS) for video stream";
  }

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
        // Wall-clock drift correction for this track (no-op pass-through
        // if SWR allocation fails).
        m_driftCorrectors[i] = std::make_unique<AudioDriftCorrector>(
            audioConfig.sampleRate, audioConfig.channels);
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
            if (!m_running)
              return;
            if (trackIndex < 0 || trackIndex >= kAudioTrackCount)
              return;
            if (!m_audioEncoders[trackIndex])
              return;

            EncodedPacket packet;
            std::lock_guard<std::mutex> lock(m_audioEncoderMutexes[trackIndex]);
            const int64_t nowMs = steadyNowUs();
            if (!m_videoStarted) {
              if (m_sessionStartMs >= 0 &&
                  nowMs - m_sessionStartMs > kVideoWaitTimeoutMs) {
                m_videoStarted = true; // portal never delivered; record on
              } else {
                return; // drop pre-video audio: unpairable head content
              }
            }
            // First feed past the gate starts this track's timestamps at
            // the video-relative offset (late tracks) instead of zero.
            if (!m_trackOffsetApplied[trackIndex]) {
              const int64_t videoStartMs = m_videoStartWallUs >= 0
                                               ? m_videoStartWallUs / 1000LL
                                               : nowMs;
              const int64_t skewMs = nowMs - videoStartMs;
              if (skewMs > 0 && m_audioMixer) {
                m_audioEncoders[trackIndex]->setStartOffsetSamples(
                    skewMs * m_audioMixer->sampleRate() / 1000LL);
              }
              m_trackOffsetApplied[trackIndex] = true;
            }
            // Lock the track to the wall clock before encoding (timestamps
            // count corrected samples, so they track video, not the
            // sound-card crystal).
            const float *fed = samples;
            int fedFrames = frameCount;
            if (auto &corr = m_driftCorrectors[trackIndex]) {
              fedFrames = corr->process(samples, frameCount, fed);
              if (fedFrames <= 0)
                return;
            }
            if (m_audioEncoders[trackIndex]->encodeAudio(fed, fedFrames,
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
        for (int i = 0; i < kAudioTrackCount; ++i) {
          m_driftCorrectors[i] = std::make_unique<AudioDriftCorrector>(
              audioConfig.sampleRate, audioConfig.channels);
        }
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
            if (!m_running || !m_mergedEncoder)
              return;
            if (trackIndex < 0 || trackIndex >= m_activeTrackCount)
              return;

            std::lock_guard<std::mutex> lock(m_mergeMutex);

            // Same origin alignment as separate mode (drops pre-video
            // audio; the merged origin is set at first mixed output
            // below, once every active track contributes).
            const int64_t nowMs = steadyNowUs();
            if (!m_videoStarted) {
              if (m_sessionStartMs >= 0 &&
                  nowMs - m_sessionStartMs > kVideoWaitTimeoutMs) {
                m_videoStarted = true;
              } else {
                return;
              }
            }

            const float *fed = samples;
            int fedFrames = frameCount;
            if (auto &corr = m_driftCorrectors[trackIndex]) {
              fedFrames = corr->process(samples, frameCount, fed);
              if (fedFrames <= 0)
                return;
            }
            const int incoming = fedFrames * channels;
            auto &buf = m_trackAccumulators[trackIndex];
            buf.insert(buf.end(), fed, fed + incoming);

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

              // Origin alignment at first mixed output (all tracks
              // contributing): late starts shift forward instead of
              // baking in the skew.
              if (!m_mergedOffsetApplied) {
                const int64_t outMs = steadyNowUs();
                const int64_t videoStartMs = m_videoStartWallUs >= 0
                                                 ? m_videoStartWallUs / 1000LL
                                                 : outMs;
                const int64_t skewMs = outMs - videoStartMs;
                if (skewMs > 0 && m_audioMixer) {
                  m_mergedEncoder->setStartOffsetSamples(
                      skewMs * m_audioMixer->sampleRate() / 1000LL);
                }
                m_mergedOffsetApplied = true;
              }

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
  // Replace direct queuing with FrameDispatcher-based routing if available
  if (!m_frameDispatcher) {
    // fallback to original behavior
    m_capture->setFrameCallback([this](const VideoFrame &frame) {
      if (!m_running)
        return;

      std::lock_guard<std::mutex> lock(m_queueMutex);
      if (m_videoQueue.size() < MAX_QUEUE_SIZE) {
        m_videoQueue.emplace(frame);
        m_queueCv.notify_one();
      } else {
        qWarning() << "Video queue full, dropping frame";
      }
    });
  } else {
    m_capture->setFrameCallback([this](const VideoFrame &frame) {
      if (!m_running)
        return;

      // Latch the video wall clock: audio fed before this is dropped and
      // late tracks get a one-time encoder origin shift (see the audio
      // callbacks below), so startup skew never bakes into the file.
      // Unit is microseconds to match the audio-side steady clock.
      if (!m_videoStarted) {
        const int64_t wallUs = steadyNowUs() * 1000LL;
        int64_t expected = -1;
        m_videoStartWallUs.compare_exchange_strong(expected, wallUs);
        m_videoStarted = true;
      }

      // Build a FrameDescriptor and dispatch (non-blocking)
      FrameDescriptor fd;
      if (frame.isDmaBuf && frame.dmaBufFd >= 0) {
        fd.fd = ::dup(frame.dmaBufFd);
        if (fd.fd == -1) {
          return; // cannot dup, drop
        }
        fd.width = frame.width;
        fd.height = frame.height;
        fd.format = frame.drmFormat;
        fd.pixelFormat = frame.pixelFormat;
        fd.drmModifier = frame.drmModifier;
        fd.dmaBufStride = frame.dmaBufStride;
        fd.dmaBufOffset = frame.dmaBufOffset;
        fd.pts = frame.timestamp;
        m_frameDispatcher->dispatch(fd);
      } else if (!frame.isDmaBuf && frame.data && frame.stride > 0 && frame.height > 0) {
        // Latch the video wall clock (see above).
        if (!m_videoStarted) {
          const int64_t wallUs = steadyNowUs() * 1000LL;
          int64_t expected = -1;
          m_videoStartWallUs.compare_exchange_strong(expected, wallUs);
          m_videoStarted = true;
        }
        // Software frame path: build a QueuedFrame and enqueue directly
        QueuedFrame qf(frame);
        if (qf.data) {
          pushQueuedFrame(std::move(qf));
        }
      } else {
        return;
      }
    });
  }

  if (!m_capture->startCapture()) {
    m_running = false;
    emit errorOccurred("Failed to start video capture");
    return false;
  }

  if (m_audioMixer) {
    m_audioMixer->startAll();
  }

  if (!m_gameHints.isEmpty() || m_gamePid != 0 || !m_voiceChatApp.isEmpty()) {
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

bool RecordingSession::pushQueuedFrame(QueuedFrame &&frame) {
  if (!m_running) {
    qDebug() << "pushQueuedFrame: rejected (not running)";
    return false;
  }
  std::lock_guard<std::mutex> lock(m_queueMutex);
  if (m_videoQueue.size() < MAX_QUEUE_SIZE) {
    m_videoQueue.push(std::move(frame));
    // Success logs at 60fps; throttle unless TROPHY_VIDEO_DEBUG is set.
    static std::atomic<uint64_t> s_pushed{0};
    const uint64_t n = ++s_pushed;
    if (qEnvironmentVariableIsSet("TROPHY_VIDEO_DEBUG") || n == 1 ||
        n % 600 == 0) {
      qDebug() << "pushQueuedFrame: enqueued. queue size:" << m_videoQueue.size();
    }
    m_queueCv.notify_one();
    return true;
  }
  qDebug() << "pushQueuedFrame: queue full, dropping frame";
  return false;
}

void RecordingSession::stop(bool keepCaptureSource) {
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
    if (!keepCaptureSource)
      m_capture->stopCapture();
    // Always detach, even when the capture keeps running: finalizeSegment
    // stops with keepCaptureSource=true and then destroys this session
    // while the stream is still live. A dangling [this] callback firing on
    // the next PipeWire frame locks a destroyed mutex (std::system_error
    // -> terminate -> SIGABRT on the stream thread). Seen live on every
    // clip-key press.
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

void RecordingSession::reconcileAppTrack(int track, uint32_t pid,
                                          const QStringList &hints,
                                          bool *isFallback) {
  if (track < 0 || track >= kAudioTrackCount || !m_audioMixer)
    return;

  // Fresh lookup: PID first (exact), then name hints in order.
  uint32_t nodeId = PW_ID_ANY;
  QString nodeName, nodeSerial;
#ifdef HAVE_PIPEWIRE
  if (pid != 0) {
    nodeId = PipeWireAudioSource::findNodeByPid(pid, &nodeName, &nodeSerial);
  }
  if (nodeId == PW_ID_ANY) {
    for (const QString &hint : hints) {
      if (hint.isEmpty())
        continue;
      nodeId = PipeWireAudioSource::findNodeByProcessName(hint, &nodeName,
                                                          &nodeSerial);
      if (nodeId != PW_ID_ANY)
        break;
    }
  }
#else
  Q_UNUSED(pid);
  Q_UNUSED(hints);
#endif

  if (nodeId == PW_ID_ANY) {
    // No live node. A previous bind died with its stream (Electron /
    // Proton streams are recreated constantly) — forget it so the next
    // appearance re-binds instead of being skipped as "connected".
    if (m_sourceConnected[track]) {
      qDebug() << "Audio node vanished for track" << track
               << "- will re-bind on return";
      m_sourceConnected[track] = false;
      m_boundSerial[track].clear();
    }
    return;
  }

  if (m_sourceConnected[track]) {
    if (m_boundSerial[track].isEmpty()) {
      if (nodeSerial.isEmpty())
        return; // serial-less node we can't churn-check; leave it alone
      // Manager bound this node at build without knowing the serial —
      // adopt it so later generations read as new, not same.
      m_boundSerial[track] = nodeSerial;
      if (isFallback)
        *isFallback = false;
      return;
    }
    if (nodeSerial.isEmpty() || nodeSerial == m_boundSerial[track])
      return; // same generation (or uncheckable) — nothing to do
    qDebug() << "Audio node recreated for track" << track << "- re-binding";
  }

  // (Re)bind — replaces monitor fallback or a dead stream generation.
  // Startup skew is handled downstream instead (audio gate + encoder
  // start offsets), so binds happen the moment a node is usable.
  m_audioMixer->addSource(PipeWireAudioSource::createApplicationSource(
                              nodeId, nodeName, nodeSerial),
                          track);
  m_sourceConnected[track] = true;
  m_boundSerial[track] = nodeSerial;
  if (isFallback)
    *isFallback = false;
  qDebug() << "Audio node bound for track" << track << ":"
           << (nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
}

void RecordingSession::checkForNewAudioNodes() {
  if (!m_running || !m_audioMixer) {
    return;
  }

  // Game track: PID + hints; monitor fallback never counts as bound.
  if (m_gamePid != 0 || !m_gameHints.isEmpty())
    reconcileAppTrack(0, m_gamePid, m_gameHints, &m_gameIsFallback);

  if (!m_voiceChatApp.isEmpty()) {
    if (m_voiceTrack >= 0 && m_voiceTrack < kAudioTrackCount) {
      // Reserved slot (separate mode always reserves it at build).
      reconcileAppTrack(m_voiceTrack, 0, {m_voiceChatApp}, nullptr);
    } else if (!m_separateAudioTracks &&
               m_activeTrackCount < kAudioTrackCount) {
      // Merged mode can't reserve a voice slot at build (an empty slot
      // would stall the mix gate forever), so adopt the next free slot if
      // the stream exists now. Never clobbers mic: the slot is always fresh.
      const int probe = m_activeTrackCount;
      reconcileAppTrack(probe, 0, {m_voiceChatApp}, nullptr);
      if (m_sourceConnected[probe]) {
        m_voiceTrack = probe;
        {
          std::lock_guard<std::mutex> lock(m_mergeMutex);
          if (m_activeTrackCount <= probe)
            m_activeTrackCount = probe + 1;
        }
        qDebug() << "Voice adopted merged slot" << probe;
      }
    }
    // (merged + unslotted + absent: skip — binding there would evict mic)
  }

  // The timer intentionally runs for the whole session: app streams churn
  // (voice join/leave, Proton audio restart) and a pin to a dead serial
  // delivers silence forever otherwise.
}

} // namespace TrophyBackend