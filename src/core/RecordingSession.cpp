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
#include <libavutil/mathematics.h>
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
  // std::atomic default-constructs indeterminate: pin the latches here
  // (start() re-pins every session, but no load may precede a store).
  for (auto &applied : m_trackOffsetApplied)
    applied.store(false);
  m_mergedOffsetApplied.store(false);

  m_firstMergedInputWallUs = -1;
  m_videoStartWallUs.store(-1);
  m_audioLookupTimer = new QTimer(this);
  m_audioLookupTimer->setInterval(10);
  connect(m_audioLookupTimer, &QTimer::timeout, this,
          &RecordingSession::deliverAudioLookups);
  m_stopTimer = new QTimer(this);
  m_stopTimer->setInterval(10);
  connect(m_stopTimer, &QTimer::timeout, this, &RecordingSession::pollStopWorker);
}

RecordingSession::~RecordingSession() {
  stop();
  // Registry walks capture only their query values. The shared mixer outlives
  // sessions (including DVR handoffs), so it can own discarded work without
  // delaying a rotation or allowing a worker to access a destroyed session.
  retireAudioLookups();
}

void RecordingSession::retireAudioLookups() {
  ++m_audioLookupGeneration;
  m_audioLookupTimer->stop();
  if (m_audioMixer)
    for (int track = 0; track < kAudioTrackCount; ++track)
      if (m_audioLookups[track].worker.valid())
        m_audioMixer->retainDiscardedNodeLookup(
            std::move(m_audioLookups[track].worker), track);
}

int64_t RecordingSession::steadyNowMs() {
  // Milliseconds; matches m_sessionStartMs units (×1000 at use sites
  // needing microseconds).
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

RecordingSession::AudioRoutingState RecordingSession::audioRoutingState() const {
  return {m_activeTrackCount.load(), m_voiceTrack, m_gameIsFallback,
          m_sourceConnected, m_boundSerial};
}

void RecordingSession::inheritAudioRouting(const RecordingSession &previous) {
  const auto routing = previous.audioRoutingState();
  m_activeTrackCount.store(routing.activeTrackCount);
  m_voiceTrack = routing.voiceTrack;
  m_gameIsFallback = routing.gameIsFallback;
  m_sourceConnected = routing.connected;
  m_boundSerial = routing.boundSerial;
}

bool RecordingSession::start() {
  if (m_stopWorker.valid())
    return false;
  if (m_running) {
    qWarning() << "Recording session already running";
    return false;
  }
  ++m_audioLookupGeneration;

  qDebug() << "Starting recording session to:" << m_outputPath;

  // Initialize state BEFORE starting any threads or callbacks
  m_frameCount = 0;
  m_bytesWritten = 0;
  m_outputFailed = false;
  m_outputFinalized = false;
  m_running = true;
  // A/V origin alignment baseline (see header): session wall start for
  // the video timeout, origins reset per session.
  m_sessionStartMs = nowUs() / 1000;
  m_videoStartWallUs = -1;
  m_firstMergedInputWallUs = -1;
  m_lastMergedInputWallUs = -1;
  m_observedMergedInputWallUs.store(-1);
  m_mergedOffsetFrames = 0;
  m_mergedSubmittedFrames = 0;
  m_mergedClocked = false;
  m_clockInputTargetFrames = 0;
  m_clockBufferStartFrames.fill(-1);
  m_clockTrackNextFrames.fill(-1);
  m_clockTrackArrivalUs.fill(-1);
  for (auto &buffer : m_trackAccumulators) buffer.clear();
  m_firstCapturedVideoTimestampUs.store(-1);
  m_capturedVideoTimelineUs.store(-1);
  m_videoFrameDurationUs = 1000000LL / std::max(1, m_encoder->fps());
  for (auto &applied : m_trackOffsetApplied)
    applied.store(false);
  m_mergedOffsetApplied.store(false);

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

      m_audioCallbackRegistration = m_audioMixer->setTrackAudioCallback(
          [this](int trackIndex, const float *samples, int frameCount,
                 int sampleRate, int channels) {
            if (!m_running)
              return;
            if (trackIndex < 0 || trackIndex >= kAudioTrackCount)
              return;

            EncodedPacket packet;
            std::lock_guard<std::mutex> lock(m_audioEncoderMutexes[trackIndex]);
            // Null-check inside the mutex: a future reset/teardown of the
            // encoder array can't slip in between the check and the use.
            if (!m_audioEncoders[trackIndex])
              return;

            // Drop audio until the first video frame exists (unpairable
            // head content); timeout force-opens so a dead portal can't
            // mute audio.
            if (m_videoStartWallUs.load() < 0 &&
                (m_sessionStartMs < 0 ||
                 nowUs() / 1000 - m_sessionStartMs <= kVideoWaitTimeoutMs)) {
              return;
            }
            // Latch audio origin; a track starting after the first video
            // frame gets a one-time encoder offset so origins agree.
            const int64_t inputNowUs = nowUs();
            if (!m_trackOffsetApplied[trackIndex].load()) {
              const int64_t videoStart = m_videoStartWallUs.load();
              if (videoStart >= 0 && inputNowUs > videoStart && m_audioMixer) {
                m_audioEncoders[trackIndex]->setStartOffsetSamples(
                    (inputNowUs - videoStart) * m_audioMixer->sampleRate() /
                    1000000LL);
              }
              m_trackOffsetApplied[trackIndex].store(true);
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
                if (writeOutputPacket(packet)) {
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

      m_audioCallbackRegistration = m_audioMixer->setTrackAudioCallback(
          [this](int trackIndex, const float *samples, int frameCount,
                 int sampleRate, int channels) {
            if (!m_running || !m_mergedEncoder)
              return;
            if (!samples || frameCount <= 0 || !m_audioMixer ||
                channels != m_audioMixer->channels() ||
                sampleRate != m_audioMixer->sampleRate())
              return;
            if (trackIndex < 0 || trackIndex >= m_activeTrackCount.load())
              return;

            const int64_t arrivalUs = nowUs();
            if (m_videoStartWallUs.load() >= 0 ||
                arrivalUs / 1000 - m_sessionStartMs > kVideoWaitTimeoutMs) {
              auto observed = m_observedMergedInputWallUs.load();
              while (arrivalUs > observed &&
                     !m_observedMergedInputWallUs.compare_exchange_weak(observed, arrivalUs)) {}
            }
            std::lock_guard<std::mutex> lock(m_mergeMutex);

            // Same gate as separate mode: no audio before first video
            // (timeout force-opens; see above). This also bounds the
            // accumulators when video never arrives.
            if (m_videoStartWallUs.load() < 0 &&
                (m_sessionStartMs < 0 ||
                  arrivalUs / 1000 - m_sessionStartMs <= kVideoWaitTimeoutMs)) {
              return;
            }

            const float *fed = samples;
            int fedFrames = frameCount;
            if (m_mergedClocked && frameCount > kMergedClockBufferedFrames) {
              fed += static_cast<size_t>(frameCount - kMergedClockBufferedFrames) * channels;
              fedFrames = kMergedClockBufferedFrames;
            }
            if (auto &corr = m_driftCorrectors[trackIndex]) {
              fedFrames = corr->process(fed, fedFrames, fed);
              if (fedFrames <= 0)
                return;
            }
            if (m_firstMergedInputWallUs < 0)
              m_firstMergedInputWallUs = arrivalUs;
            m_lastMergedInputWallUs = std::max(m_lastMergedInputWallUs, arrivalUs);
            if (m_mergedClocked) {
              appendClockedMergedAudio(trackIndex, fed, fedFrames, channels, arrivalUs);
              m_queueCv.notify_one();
              return;
            }
            m_clockTrackArrivalUs[trackIndex] = arrivalUs;
            // Append at most one AAC block before draining, so even a large
            // native buffer cannot temporarily grow an accumulator unbounded.
            for (int offset = 0; offset < fedFrames;) {
              const int frames = std::min(1024, fedFrames - offset);
              auto &buf = m_trackAccumulators[trackIndex];
              buf.insert(buf.end(), fed + offset * channels,
                         fed + (offset + frames) * channels);
              offset += frames;
              drainMergedAudio(channels);
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

      // Latch the video wall clock on the first frame (same as the
      // dispatcher path below). DVR sessions have no dispatcher, so
      // without this the audio gate never opens on video and every
      // segment drops its first 10s of audio to the gate timeout —
      // clips then carry full video with ~10s-short audio.
      noteCapturedVideo(frame);

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

      // Latch the video wall clock on the first frame. Audio before this
      // is dropped downstream (unpairable head content); late audio
      // tracks offset to this origin instead.
      noteCapturedVideo(frame);

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
    // start installed a raw-session callback above. stop() takes its idle
    // branch after this failure, so detach here while the session is alive.
    m_capture->setFrameCallback(nullptr);
    emit errorOccurred("Failed to start video capture");
    return false;
  }

  if (m_audioMixer) {
    // Start streams that are not already running. DVR switches keep the
    // mixer's PipeWire sources alive across the handoff; restarting them
    // would reopen an A/V gap on every rotation.
    m_audioMixer->ensureSourcesStartedAsync();
  }

  if (m_audioMixer) {
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

void RecordingSession::stop(bool keepCaptureSource, bool handoffRouting) {
  if (m_stopWorker.valid()) {
    m_stopWorker.get();
    m_stopTimer->stop();
    if (!m_asyncHandoff && m_audioMixer)
      m_audioMixer->stopAll();
    return;
  }
  if (!retireStopRouting(keepCaptureSource, handoffRouting)) {
    if (m_audioMixer && m_audioCallbackRegistration) {
      m_audioMixer->drainTrackAudioCallback(m_audioCallbackRegistration);
      m_audioCallbackRegistration.reset();
    }
    return;
  }
  finishStopNative();
  if (!handoffRouting && m_audioMixer)
    m_audioMixer->stopAll();
}

void RecordingSession::requestStop(bool keepCaptureSource, bool handoffRouting) {
  if (m_stopWorker.valid())
    return;
  const bool active = retireStopRouting(keepCaptureSource, handoffRouting);
  retireAudioLookups();
  if (!active && !keepCaptureSource && m_capture) {
    m_finishCaptureStop = true;
    m_capture->prepareStopCapture();
  }
  m_asyncHandoff = handoffRouting;
  if (!handoffRouting && m_audioMixer)
    m_audioMixer->stopSourcesAsync();
  // Partial-start failures still own native capture/muxer resources. Drain
  // and finalize those here too, rather than deferring their first cleanup
  // to a GUI-thread destructor. Empty encoder/queue slots are safe no-ops.
  m_stopWorker = std::async(std::launch::async, [this] { finishStopNative(); });
  m_stopTimer->start();
}

void RecordingSession::pollStopWorker() {
  if (!m_stopWorker.valid()) { m_stopTimer->stop(); return; }
  if (m_stopWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  if (!m_asyncHandoff && m_audioMixer)
    for (int track = 0; track < AudioMixer::kTrackCount; ++track)
      if (m_audioMixer->sourceOperationPending(track))
        return;
  m_stopWorker.get();
  m_stopTimer->stop();
  emit stopFinished();
}

bool RecordingSession::retireStopRouting(bool keepCaptureSource, bool handoffRouting) {
  ++m_audioLookupGeneration;
  m_audioLookupTimer->stop();
  if (!m_running) {
    return false;
  }

  qDebug() << "Stopping recording session...";

  if (m_audioNodeCheckTimer) {
    m_audioNodeCheckTimer->stop();
    m_audioNodeCheckTimer->deleteLater();
    m_audioNodeCheckTimer = nullptr;
  }

  if (m_capture) {
    m_finishCaptureStop = !keepCaptureSource;
    if (m_finishCaptureStop)
      m_capture->prepareStopCapture();
    // Detach the frame callback unless a successor session already owns
    // the live routing (DVR make-before-break switch): nulling it here
    // would steal the new session's frames. A dangling [this] callback
    // firing on the next PipeWire frame locks a destroyed mutex
    // (std::system_error -> terminate -> SIGABRT on the stream thread),
    // so the non-handoff path always detaches. Seen live on every
    // clip-key press.
    if (!handoffRouting && keepCaptureSource)
      m_capture->setFrameCallback(nullptr);
  }

  m_running = false;
  if (m_audioMixer) {
    if (!handoffRouting)
      m_audioMixer->setTrackAudioCallback(nullptr);
    // During a DVR handoff the successor's registration is already
    // published. Drain only this session's registration, leaving the
    // successor untouched, before flushing/finalizing this session's muxer.
    if (m_audioCallbackRegistration) {
      std::lock_guard<std::mutex> lock(m_audioCallbackRegistration->mutex);
      m_audioCallbackRegistration->accepting = false;
    }
  }
  m_queueCv.notify_all();
  return true;
}

void RecordingSession::finishStopNative() {
  if (m_finishCaptureStop && m_capture) {
    m_capture->finishStopCapture();
    m_capture->setFrameCallback(nullptr);
  }
  if (m_audioMixer) {
    m_audioMixer->drainTrackAudioCallback(m_audioCallbackRegistration);
    m_audioCallbackRegistration.reset();
  }

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
        writeOutputPacket(pkt);
      }
    }
  }

  if (m_mergedEncoder) {
    // An entered input callback may have finished after the video loop exited.
    // Producers are drained now; settle its final timeline on this owned stop
    // worker, with the same per-iteration budget as live clock catch-up.
    while (advanceMergedClock(true)) {}
    // Callback retirement above makes this the sole accumulator consumer.
    // Preserve the bounded grace tail at stop/handoff instead of dropping it.
    {
      std::lock_guard<std::mutex> lock(m_mergeMutex);
      drainMergedAudio(m_audioMixer->channels(), true);
      // Clocked input may include a bounded future cadence allowance. Stop
      // truncates it to the captured/observed timeline; never replay that
      // retained tail into a later start of this object.
      if (m_mergedClocked)
        for (auto &buffer : m_trackAccumulators) buffer.clear();
    }
    std::vector<EncodedPacket> flushed;
    m_mergedEncoder->flush(flushed);
    for (auto &pkt : flushed) {
      pkt.streamIndex = 1;
      writeOutputPacket(pkt);
    }
  }

  if (m_muxer) {
    if (!m_muxer->finalize())
      m_outputFailed = true;
    m_outputFinalized = true;
    
    m_bytesWritten = m_muxer->bytesWritten();
  }

  qDebug() << "Recording session stopped. Frames:" << m_frameCount
          << "Size:" << m_bytesWritten << "bytes";
}

void RecordingSession::encodingLoop() {
  qDebug() << "Encoding thread started";

  bool catchup = false;
  while (true) {
    QueuedFrame frame(VideoFrame{nullptr, 0, 0, 0, 0, 0});
    bool haveFrame = false;
    {
      std::unique_lock<std::mutex> lock(m_queueMutex);
      if (!catchup)
        m_queueCv.wait_for(lock, std::chrono::milliseconds(20),
                          [this] { return !m_videoQueue.empty() || !m_running; });
      if (!m_videoQueue.empty()) {
        frame = std::move(m_videoQueue.front());
        m_videoQueue.pop();
        haveFrame = true;
      }
    }

    EncodedPacket packet;
    if (haveFrame && m_encoder->encodeFrame(frame, packet)) {
      packet.streamIndex = 0;
      if (writeOutputPacket(packet)) {
        m_bytesWritten = m_muxer->bytesWritten();
      }
      m_frameCount++;
    }
    catchup = advanceMergedClock(!m_running.load());
    if (!haveFrame && !m_running && !catchup)
      break;
  }

  std::vector<EncodedPacket> flushedPackets;
  if (m_encoder && m_encoder->flush(flushedPackets) > 0) {
    for (auto &pkt : flushedPackets) {
      pkt.streamIndex = 0;
      writeOutputPacket(pkt);
    }
  }

  qDebug() << "Encoding thread stopped";
}

void RecordingSession::noteCapturedVideo(const VideoFrame &frame) {
  const int64_t wall = nowUs();
  int64_t expected = -1;
  m_videoStartWallUs.compare_exchange_strong(expected, wall);
  if (frame.timestamp < 0) return;
  expected = -1;
  m_firstCapturedVideoTimestampUs.compare_exchange_strong(expected, frame.timestamp);
  const auto first = m_firstCapturedVideoTimestampUs.load();
  if (frame.timestamp < first) return;
  const int64_t relative = frame.timestamp - first;
  const int64_t elapsed = std::max<int64_t>(0, wall - m_videoStartWallUs.load());
  // Do not synthesize years of PCM for corrupt/far-future capture timestamps.
  if (relative > elapsed && relative - elapsed > 1000000) return;
  if (relative > INT64_MAX - m_videoFrameDurationUs) return;
  const int64_t end = relative + m_videoFrameDurationUs;
  auto old = m_capturedVideoTimelineUs.load();
  while (end > old && !m_capturedVideoTimelineUs.compare_exchange_weak(old, end)) {}
}

int64_t RecordingSession::mergedTimelineFrames(int64_t us) const {
  if (us <= 0 || !m_audioMixer) return 0;
  return std::max<int64_t>(0, av_rescale_rnd(us, m_audioMixer->sampleRate(),
                                           1000000, AV_ROUND_DOWN));
}

int64_t RecordingSession::mergedVideoTargetFrames() const {
  return mergedTimelineFrames(m_capturedVideoTimelineUs.load());
}

void RecordingSession::applyMergedOffset() {
  if (m_mergedOffsetApplied.load()) return;
  const int64_t videoStart = m_videoStartWallUs.load();
  if (videoStart >= 0 && m_firstMergedInputWallUs > videoStart)
    m_mergedOffsetFrames = mergedTimelineFrames(m_firstMergedInputWallUs - videoStart);
  m_mergedEncoder->setStartOffsetSamples(m_mergedOffsetFrames);
  m_mergedOffsetApplied.store(true);
}

bool RecordingSession::advanceMergedClock(bool finalDrain) {
  if (m_outputFinalized.load() || m_separateAudioTracks || !m_mergedEncoder || !m_audioMixer ||
      m_videoStartWallUs.load() < 0 || m_capturedVideoTimelineUs.load() < 0 ||
      m_activeTrackCount.load() <= 0)
    return false;
  std::lock_guard lock(m_mergeMutex);
  if (!m_mergedClocked) {
    const int64_t last = m_observedMergedInputWallUs.load();
    const int64_t reference = last >= 0 ? last : m_videoStartWallUs.load();
    if (nowUs() - reference <= kMergedClockGraceUs) return false;
    const int64_t grace = finalDrain ? 0 : mergedTimelineFrames(kMergedClockGraceUs);
    int64_t offset = m_mergedOffsetFrames;
    if (!m_mergedOffsetApplied && m_firstMergedInputWallUs > m_videoStartWallUs.load())
      offset = mergedTimelineFrames(m_firstMergedInputWallUs - m_videoStartWallUs.load());
    if (mergedVideoTargetFrames() - grace < offset + m_mergedSubmittedFrames + 1024)
      return false;
    applyMergedOffset();
    const int64_t cursor = m_mergedOffsetFrames + m_mergedSubmittedFrames;
    for (int track = 0; track < kAudioTrackCount; ++track) {
      auto &buffer = m_trackAccumulators[track];
      const size_t frames = buffer.size() / m_audioMixer->channels();
      const size_t skip = frames > kMergedClockBufferedFrames
                              ? frames - kMergedClockBufferedFrames : 0;
      if (skip) buffer.erase(buffer.begin(), buffer.begin() + skip * m_audioMixer->channels());
      m_clockBufferStartFrames[track] = cursor + skip;
      m_clockTrackNextFrames[track] = cursor + frames;
    }
    m_mergedClocked = true;
  }
  return drainClockedMergedAudio(m_audioMixer->channels(), finalDrain);
}

void RecordingSession::appendClockedMergedAudio(int track, const float *samples,
                                               int frames, int channels,
                                               int64_t arrivalUs) {
  const int64_t cursor = m_mergedOffsetFrames + m_mergedSubmittedFrames;
  const int64_t wallEnd = mergedTimelineFrames(
      std::max<int64_t>(0, arrivalUs - m_videoStartWallUs.load()));
  const int64_t grace = mergedTimelineFrames(kMergedClockGraceUs);
  int64_t start = wallEnd - frames;
  const int64_t last = m_clockTrackArrivalUs[track];
  const int64_t next = m_clockTrackNextFrames[track];
  if (frames < kMergedClockBufferedFrames && last >= 0 && arrivalUs >= last &&
      arrivalUs - last <= kMergedClockGraceUs && next >= cursor &&
      next >= start - grace && next <= wallEnd + grace)
    start = next; // Trust short continuous delivery cadence, not scheduler jitter.
  const int64_t end = start + frames;
  m_clockTrackArrivalUs[track] = std::max(last, arrivalUs);
  m_clockTrackNextFrames[track] = end;
  m_clockInputTargetFrames = std::max(m_clockInputTargetFrames, wallEnd);
  const int64_t cap = std::max(mergedVideoTargetFrames(), wallEnd) + grace;
  const int64_t keepStart = std::max({start, cursor, end - kMergedClockBufferedFrames});
  const int64_t keepEnd = std::min(end, cap);
  if (keepEnd <= keepStart) return;
  auto &buffer = m_trackAccumulators[track];
  auto &base = m_clockBufferStartFrames[track];
  if (buffer.empty()) base = keepStart;
  int64_t bufferedEnd = base + buffer.size() / channels;
  int64_t appendStart = std::max(keepStart, bufferedEnd);
  if (keepEnd <= appendStart) return;
  if (keepEnd - base > kMergedClockBufferedFrames || keepStart - bufferedEnd > grace) {
    buffer.clear(); base = keepStart; bufferedEnd = keepStart; appendStart = keepStart;
  }
  if (appendStart > bufferedEnd)
    buffer.insert(buffer.end(), static_cast<size_t>(appendStart - bufferedEnd) * channels, 0.0f);
  buffer.insert(buffer.end(), samples + (appendStart - start) * channels,
                samples + (keepEnd - start) * channels);
}

bool RecordingSession::drainClockedMergedAudio(int channels, bool finalDrain) {
  const int64_t target = std::max(mergedVideoTargetFrames(), m_clockInputTargetFrames) -
                        (finalDrain ? 0 : mergedTimelineFrames(kMergedClockGraceUs));
  const int tracks = std::clamp(m_activeTrackCount.load(), 0, kAudioTrackCount);
  for (int block = 0; block < kMergedClockBlocksPerTick; ++block) {
    const int64_t cursor = m_mergedOffsetFrames + m_mergedSubmittedFrames;
    const int frames = static_cast<int>(std::min<int64_t>(1024, target - cursor));
    if (frames <= 0 || (!finalDrain && frames < 1024)) return false;
    std::vector<float> mixed(static_cast<size_t>(frames) * channels, 0.0f);
    for (int track = 0; track < tracks; ++track) {
      auto &buffer = m_trackAccumulators[track];
      auto &base = m_clockBufferStartFrames[track];
      if (buffer.empty()) continue;
      if (base < cursor) {
        const int64_t skip = std::min<int64_t>(cursor - base, buffer.size() / channels);
        buffer.erase(buffer.begin(), buffer.begin() + skip * channels);
        base += skip;
      }
      const int64_t offset = base - cursor;
      if (buffer.empty() || offset >= frames) continue;
      const int64_t count = std::min<int64_t>(frames - offset, buffer.size() / channels);
      for (int64_t sample = 0; sample < count * channels; ++sample)
        mixed[offset * channels + sample] += buffer[sample];
      buffer.erase(buffer.begin(), buffer.begin() + count * channels);
      base += count;
    }
    for (auto &sample : mixed) sample = std::clamp(sample, -1.0f, 1.0f);
    EncodedPacket packet;
    m_mergedSubmittedFrames += frames;
    if (m_mergedEncoder->encodeAudio(mixed.data(), frames, packet)) {
      packet.streamIndex = 1;
      if (writeOutputPacket(packet)) m_bytesWritten = m_muxer->bytesWritten();
    }
  }
  return target > m_mergedOffsetFrames + m_mergedSubmittedFrames + (finalDrain ? 0 : 1023);
}

bool RecordingSession::writeOutputPacket(const EncodedPacket &packet) {
  if (!m_muxer || !m_muxer->writePacket(packet)) {
    m_outputFailed = true;
    return false;
  }
  return true;
}

void RecordingSession::drainMergedAudio(int channels, bool flushTail) {
  if (m_mergedClocked) {
    drainClockedMergedAudio(channels, flushTail);
    return;
  }
  const size_t needed = 1024 * channels;
  // Up to 100ms of source-delivery skew is tolerated. Once a contributing
  // track falls behind, silence advances that slot instead of blocking every
  // track forever. Output remains driven by live, drift-corrected sample time.
  const size_t waitSamples =
      static_cast<size_t>(std::max(1024, m_audioMixer->sampleRate() / 10)) * channels;
  const int activeTracks = std::clamp(m_activeTrackCount.load(), 0, kAudioTrackCount);
  while (true) {
    bool allReady = true;
    size_t largest = 0;
    for (int i = 0; i < activeTracks; ++i) {
      largest = std::max(largest, m_trackAccumulators[i].size());
      if (m_trackAccumulators[i].size() < needed)
        allReady = false;
    }
    if (largest == 0 || (!flushTail &&
        (largest < needed || (!allReady && largest < waitSamples))))
      return;
    std::vector<float> mixed(needed, 0.0f);
    for (int i = 0; i < activeTracks; ++i) {
      auto &buf = m_trackAccumulators[i];
      const size_t available = std::min(needed, buf.size());
      for (size_t s = 0; s < available; ++s)
        mixed[s] += buf[s];
      buf.erase(buf.begin(), buf.begin() + available);
    }
    for (auto &v : mixed)
      v = std::clamp(v, -1.0f, 1.0f);
    applyMergedOffset();
    m_mergedSubmittedFrames += 1024;
    EncodedPacket packet;
    if (m_mergedEncoder->encodeAudio(mixed.data(), 1024, packet)) {
      packet.streamIndex = 1;
      if (writeOutputPacket(packet))
        m_bytesWritten = m_muxer->bytesWritten();
    }
  }
}

void RecordingSession::reconcileAppTrack(int track, uint32_t pid,
                                          const QStringList &hints,
                                          bool *isFallback) {
  if (track < 0 || track >= kAudioTrackCount || !m_audioMixer)
    return;

  if (m_audioMixer->discardedNodeLookupPending(track))
    return; // A retired session's walk still owns this track's query slot.

  auto &operation = m_audioLookups[track];
  if (operation.worker.valid()) {
    m_audioLookupTimer->start();
    return; // At most one registry walk per track, including stale work.
  }
  operation.generation = m_audioLookupGeneration;
  operation.game = isFallback != nullptr;
  operation.worker = std::async(std::launch::async,
                               [lookup = m_audioNodeLookup, pid, hints] {
    return lookup(pid, hints);
  });
  m_audioLookupTimer->start();
}

RecordingSession::AudioNodeResult
RecordingSession::lookupAudioNode(uint32_t pid, const QStringList &hints) {

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

  return {nodeId, nodeName, nodeSerial};
}

void RecordingSession::deliverAudioLookups() {
  bool pending = false;
  for (int track = 0; track < kAudioTrackCount; ++track) {
    auto &operation = m_audioLookups[track];
    if (!operation.worker.valid())
      continue;
    if (operation.worker.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready) {
      pending = true;
      continue;
    }
    const auto result = operation.worker.get();
    if (!m_running || operation.generation != m_audioLookupGeneration)
      continue;
    applyAppTrackNode(track, result.id, result.name, result.serial,
                      operation.game ? &m_gameIsFallback : nullptr);
    if (!operation.game && m_voiceTrack < 0 && !m_separateAudioTracks &&
        m_sourceConnected[track]) {
      m_voiceTrack = track;
      if (m_activeTrackCount.load() <= track)
        m_activeTrackCount.store(track + 1);
    }
  }
  if (!pending)
    m_audioLookupTimer->stop();
}

void RecordingSession::applyAppTrackNode(int track, uint32_t nodeId,
                                         const QString &nodeName,
                                         const QString &nodeSerial,
                                         bool *isFallback) {
  if (track < 0 || track >= kAudioTrackCount || !m_audioMixer)
    return;
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

  // A terminal stream/core error can kill our capture while the target node
  // remains alive with the same serial. Node identity alone is not evidence
  // that this source still works; let the normal retry replace the failed leg.
  if (m_audioMixer->sourceOperationPending(track))
    return;
  if (m_sourceConnected[track] && !m_audioMixer->sourceIsCapturing(track)) {
    m_sourceConnected[track] = false;
    m_boundSerial[track].clear();
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
  m_audioMixer->addSourceAsync(PipeWireAudioSource::createApplicationSource(
                              nodeId, nodeName, nodeSerial),
                          track);
  m_sourceConnected[track] = true;
  m_boundSerial[track] = nodeSerial;
  // A new source restarts this track's origin measurement: the encoder
  // offset below is one-shot, and a stale flag would pin a previous
  // source's origin onto new content (constant file-wide skew).
  m_trackOffsetApplied[track].store(false);
  if (isFallback)
    *isFallback = false;
  qDebug() << "Audio node bound for track" << track << ":"
           << (nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
}

void RecordingSession::checkForNewAudioNodes() {
  if (!m_running || !m_audioMixer) {
    return;
  }

  // Terminal errors clear isCapturing even when a desktop/mic target remains
  // present. Retry retained inactive sources once per owner-thread timer tick;
  // healthy sources stay untouched, and idle/teardown exits above never retry.
  m_audioMixer->ensureSourcesStartedAsync();

  // Game track: PID + hints; monitor fallback never counts as bound.
  if (m_gamePid != 0 || !m_gameHints.isEmpty())
    reconcileAppTrack(0, m_gamePid, m_gameHints, &m_gameIsFallback);

  if (!m_voiceChatApp.isEmpty()) {
    if (m_voiceTrack >= 0 && m_voiceTrack < kAudioTrackCount) {
      // Reserved slot (separate mode always reserves it at build).
      reconcileAppTrack(m_voiceTrack, 0, {m_voiceChatApp}, nullptr);
    } else if (!m_separateAudioTracks &&
               m_activeTrackCount.load() < kAudioTrackCount) {
      // Merged mode can't reserve a voice slot at build (an empty slot
      // would stall the mix gate forever), so adopt the next free slot if
      // the stream exists now. Never clobbers mic: the slot is always fresh.
      const int probe = m_activeTrackCount.load();
      reconcileAppTrack(probe, 0, {m_voiceChatApp}, nullptr);
      if (m_sourceConnected[probe]) {
        m_voiceTrack = probe;
        {
          if (m_activeTrackCount.load() <= probe)
            m_activeTrackCount.store(probe + 1);
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
