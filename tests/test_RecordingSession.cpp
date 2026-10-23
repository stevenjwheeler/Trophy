/*
 * Trophy - Unit Tests
 * RecordingSession: the audio gate must open on the first video frame even
 * when no FrameDispatcher is attached. DVR segments never set a dispatcher
 * (only RecordingManager does), and the fallback capture callback used to
 * skip the video-start latch — so every DVR segment dropped its first 10s
 * of audio to the gate timeout while video ran full-span. Clips (trailing
 * windows) then carried full video with ~10s-short audio.
 */

#include <QCoreApplication>
#include <QSize>
#include <QSignalSpy>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <thread>
#include <tuple>
#include <gtest/gtest.h>

#include "audio/AudioMixer.h"
#include "audio/IAudioSource.h"
#include "audio/AudioRoutingInfo.h"
#include "capture/IVideoCaptureSource.h"
#include "core/RecordingSession.h"
#include "encoding/IEncoder.h"
#include "encoding/FFmpegEncoder.h"
#include "muxing/IMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

using namespace TrophyBackend;

namespace TrophyBackend {
class RecordingSessionTestPeer {
public:
  static void clock(RecordingSession &session, std::shared_ptr<std::atomic<int64_t>> now) {
    session.m_clockNowUs = [now] { return now->load(); };
  }
  static void disableDrift(RecordingSession &session) {
    for (auto &corrector : session.m_driftCorrectors) corrector.reset();
  }
  static bool tick(RecordingSession &session, bool final = false) {
    return session.advanceMergedClock(final);
  }
  static int64_t submitted(RecordingSession &session) {
    std::lock_guard lock(session.m_mergeMutex);
    return session.m_mergedSubmittedFrames;
  }
  static bool clocked(RecordingSession &session) {
    std::lock_guard lock(session.m_mergeMutex);
    return session.m_mergedClocked;
  }
  static void videoTime(RecordingSession &session, int64_t timestamp) {
    VideoFrame frame{};
    frame.timestamp = timestamp;
    session.noteCapturedVideo(frame);
  }
  static void delivered(RecordingSession &session, bool dmaBuf, int width, int height) {
    VideoFrame frame{};
    frame.width = width;
    frame.height = height;
    frame.isDmaBuf = dmaBuf;
    session.noteCapturedVideo(frame);
  }
  static bool output(RecordingSession &session, int stream, size_t bytes) {
    EncodedPacket packet;
    packet.streamIndex = stream;
    packet.data.resize(bytes);
    return session.writeOutputPacket(packet);
  }
  static bool overflowQueue(RecordingSession &session) {
    session.m_running = true;
    VideoFrame frame{};
    while (session.m_videoQueue.size() < session.MAX_QUEUE_SIZE)
      session.m_videoQueue.emplace(frame);
    const bool accepted = session.pushQueuedFrame(QueuedFrame(frame));
    session.m_running = false;
    return accepted;
  }
  static AudioMixer::TrackAudioCallbackHandle registration(
      const RecordingSession &session) {
    return session.m_audioCallbackRegistration;
  }
  static size_t accumulatedSamples(RecordingSession &session, int track) {
    std::lock_guard<std::mutex> lock(session.m_mergeMutex);
    return session.m_trackAccumulators[track].size();
  }
  static void bindKnownNode(RecordingSession &session) {
    session.m_sourceConnected[0] = true;
    session.m_boundSerial[0] = "123";
  }
  static void deliverSameNode(RecordingSession &session) {
    session.applyAppTrackNode(0, 23, "game-node", "123", nullptr);
  }
  static void missingNode(RecordingSession &session, int track, bool game) {
    session.applyAppTrackNode(track, 0xffffffffu, {}, {},
                              game ? &session.m_gameIsFallback : nullptr);
  }
  static void checkNodes(RecordingSession &session) {
    session.checkForNewAudioNodes();
  }
  static void setLookup(RecordingSession &session,
                       std::function<uint32_t()> lookup) {
    session.m_audioNodeLookup = [lookup](uint32_t, const QStringList &, bool) {
      return RecordingSession::AudioNodeResult{lookup(), "old-node", "old-serial"};
    };
  }
  static void requestLookup(RecordingSession &session) {
    session.reconcileAppTrack(0, 17, {"fake-game"}, &session.m_gameIsFallback);
  }
  static bool lookupPending(RecordingSession &session) {
    return session.m_audioLookups[0].worker.valid();
  }
  static bool lookupPendingTrack(RecordingSession &session, int track) {
    return session.m_audioLookups[track].worker.valid();
  }
  static bool connected(RecordingSession &session) {
    return session.m_sourceConnected[0];
  }
  static void deliverLookups(RecordingSession &session) {
    session.deliverAudioLookups();
  }
  static void setMergeOrigin(RecordingSession &session) {
    const int64_t nowUs = session.steadyNowMs() * 1000LL;
    session.m_videoStartWallUs.store(nowUs - 500000);
    session.m_firstMergedInputWallUs = nowUs - 450000;
  }
};
} // namespace TrophyBackend

class RecordingSessionTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new RecordingSessionTestEnvironment);

namespace {

bool haveAac() { return avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr; }

bool settleMixerSources(AudioMixer &mixer) {
  const auto pending = [&] {
    for (int track = 0; track < AudioMixer::kTrackCount; ++track)
      if (mixer.sourceOperationPending(track)) return true;
    return false;
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (pending() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return !pending();
}

class FakeCapture : public IVideoCaptureSource {
public:
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback cb) override { m_cb = std::move(cb); }
  bool startCapture() override { return startSucceeds; }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {64, 64}; }
  QString name() const override { return QStringLiteral("fake"); }
  bool startSucceeds = true;
  bool hasCallback() const { return bool(m_cb); }

  void fire(const VideoFrame &frame) {
    if (m_cb)
      m_cb(frame);
  }

private:
  FrameCallback m_cb;
};

class FakeVideoEncoder : public IEncoder {
public:
  EncoderConfig config() const override { return m_config; }
  bool initialize(const EncoderConfig &config) override {
    m_config = config;
    return true;
  }
  bool encodeFrame(const VideoFrame &, EncodedPacket &packet) override {
    if (encodeGate) encodeGate();
    ++m_frames;
    packet.data = {1};
    return emitPackets;
  }
  int flush(std::vector<EncodedPacket> &packets) override {
    if (!emitFlushPacket)
      return 0;
    EncodedPacket packet;
    packet.data = {1};
    packets.push_back(packet);
    return 1;
  }
  bool emitPackets = false;
  bool emitFlushPacket = false;
  std::function<void()> encodeGate;
  EncoderType type() const override { return EncoderType::Software_x264; }
  QString name() const override { return QStringLiteral("fake"); }
  bool isHardwareEncoder() const override { return false; }
  std::vector<uint8_t> extraData() const override { return {}; }
  int codecId() const override { return AV_CODEC_ID_H264; }
  int encodedPixelFormat() const override { return AV_PIX_FMT_YUV420P; }

  int frames() const { return m_frames.load(); }

private:
  EncoderConfig m_config;
  std::atomic<int> m_frames{0};
};

class FakeMuxer : public IMuxer {
public:
  bool initialize(const QString &outputPath) override {
    m_path = outputPath;
    return true;
  }
  int addStream(const StreamInfo &info) override {
    if (info.type == StreamInfo::Type::Audio)
      audioExtraData = info.extraData;
    return m_streams++;
  }
  bool collectAudio = false;
  bool writeSucceeds = true;
  bool finalizeSucceeds = true;
  std::vector<uint8_t> audioExtraData;
  bool writeHeader() override { return true; }
  bool writePacket(const EncodedPacket &packet) override {
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_packets;
    if (packet.streamIndex != 0) {
      ++m_audioPackets;
      m_changed.notify_all();
      m_audioPts.push_back(packet.pts);
      if (collectAudio)
        audioPayloads.push_back(packet.data);
    }
    if (packet.streamIndex != 0 && m_holdNextAudio) {
      m_holdNextAudio = false;
      m_audioEntered = true;
      m_changed.notify_all();
      m_changed.wait(lock, [this] { return m_releaseAudio; });
    }
    return writeSucceeds;
  }
  bool finalize() override {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_finalizations;
    return finalizeSucceeds;
  }
  QString outputPath() const override { return m_path; }
  int64_t bytesWritten() const override { return 0; }
  QString formatName() const override { return QStringLiteral("fake"); }

  int audioPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_audioPackets;
  }
  bool waitForAudioPackets(int count) {
    std::unique_lock lock(m_mutex);
    return m_changed.wait_for(lock, std::chrono::seconds(3), [&] {
      return m_audioPackets >= count;
    });
  }
  std::vector<int64_t> audioPts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_audioPts;
  }
  std::vector<std::vector<uint8_t>> audioPayloads;

  void holdNextAudio() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_holdNextAudio = true;
  }
  bool waitForEnteredAudio() {
    std::unique_lock<std::mutex> lock(m_mutex);
    // Watchdog only: test progress is driven by the callback rendezvous.
    return m_changed.wait_for(lock, std::chrono::seconds(5),
                             [this] { return m_audioEntered; });
  }
  void releaseAudio() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_releaseAudio = true;
    m_changed.notify_all();
  }
  int finalizations() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_finalizations;
  }

private:
  mutable std::mutex m_mutex;
  QString m_path;
  int m_streams = 0;
  int m_packets = 0;
  int m_audioPackets = 0;
  std::vector<int64_t> m_audioPts;
  std::condition_variable m_changed;
  bool m_holdNextAudio = false;
  bool m_audioEntered = false;
  bool m_releaseAudio = false;
  int m_finalizations = 0;
};

TEST(RecordingSessionHealthTest, FallbackMuteAndVideoOriginAreTruthful) {
  AudioMixer mixer(48000, 2);
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  RecordingSession session("health.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 0);
  auto now = std::make_shared<std::atomic<int64_t>>(5000000);
  RecordingSessionTestPeer::clock(session, now);
  EXPECT_EQ(session.videoPositionMs(), -1);
  EXPECT_FALSE(session.captureHealth()["hasVideo"].toBool());
  EXPECT_FALSE(RecordingSessionTestPeer::overflowQueue(session));
  EXPECT_EQ(session.captureHealth()["queueDrops"].toULongLong(), 1);
  EXPECT_EQ(session.captureHealth()["queueDepth"].toInt(), 30);
  EXPECT_EQ(session.captureHealth()["peakQueueDepth"].toInt(), 30);
  session.setGameIsFallback(true);
  session.setVoiceChatApp("Discord");
  EXPECT_TRUE(session.captureHealth()["desktopFallback"].toBool());
  EXPECT_EQ(session.captureHealth()["audio1"].toString(), "desktop-mix-unavailable");
  EXPECT_TRUE(session.captureHealth()["audio1Signal"].toMap().isEmpty());
  // Capture health names the bound identity, not just a generic "connected".
  EXPECT_EQ(session.captureHealth()["audio1Name"].toString(), "Discord");
  mixer.setSemanticTrackMuted(2, true);
  EXPECT_EQ(session.captureHealth()["audio2"].toString(), "muted");
  EXPECT_TRUE(session.captureHealth()["audio2Signal"].toMap()["muted"].toBool());
  session.setGameIsFallback(false);
  EXPECT_FALSE(session.captureHealth()["desktopFallback"].toBool());
  // Marking a node bound is not proof the source is actually capturing.
  RecordingSessionTestPeer::bindKnownNode(session);
  EXPECT_EQ(session.captureHealth()["audio0"].toString(), "unavailable");
  EXPECT_EQ(session.captureHealth()["audio1"].toString(), "unavailable");
  RecordingSessionTestPeer::videoTime(session, 100000000);
  EXPECT_EQ(session.videoPositionMs(), 0);
  now->fetch_add(12345000);
  RecordingSessionTestPeer::videoTime(session, 112345000);
  EXPECT_EQ(session.videoPositionMs(), 12345);
  EXPECT_TRUE(session.captureHealth()["hasVideo"].toBool());
  session.setDesktopAudio(true);
  EXPECT_TRUE(session.captureHealth()["desktopAudio"].toBool());
  EXPECT_FALSE(session.captureHealth()["desktopFallback"].toBool());
}

TEST(RecordingSessionHealthTest, LiveStatisticsIncludeBufferedCallsAndResetOnRestart) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  EncoderConfig config;
  config.width = 1280;
  config.height = 720;
  config.fps = 30;
  config.bitrate = 8000;
  config.useVBR = false;
  encoder.initialize(config);
  RecordingSession session("stats.mkv", &capture, &encoder, &muxer,
                           nullptr, true, 128, 0);
  EXPECT_FALSE(session.captureHealth().contains("averageEncodeMs"));
  EXPECT_FALSE(session.captureHealth().contains("encoderDrops"));
  ASSERT_TRUE(session.start());
  VideoFrame frame{};
  frame.width = 640;
  frame.height = 360;
  frame.timestamp = 1000000;
  // False means buffered, not necessarily a dropped frame.
  encoder.emitPackets = false;
  capture.fire(frame);
  session.stop();
  const auto stats = session.captureHealth();
  EXPECT_EQ(stats["deliveredFrames"].toLongLong(), 1);
  EXPECT_EQ(stats["dmaBufFrames"].toLongLong(), 0);
  EXPECT_EQ(stats["inputWidth"].toInt(), 640);
  EXPECT_EQ(stats["inputHeight"].toInt(), 360);
  EXPECT_EQ(stats["outputWidth"].toInt(), 1280);
  EXPECT_EQ(stats["outputHeight"].toInt(), 720);
  EXPECT_EQ(stats["targetFps"].toInt(), 30);
  EXPECT_EQ(stats["bitrateKbps"].toInt(), 8000);
  EXPECT_EQ(stats["rateControlMode"].toString(), "CBR");
  EXPECT_EQ(stats["videoPackets"].toLongLong(), 0);
  EXPECT_EQ(stats["queueDrops"].toLongLong(), 0);
  EXPECT_TRUE(stats.contains("averageEncodeMs"));
  EXPECT_GE(stats["averageEncodeMs"].toDouble(), 0);
  ASSERT_TRUE(session.start());
  EXPECT_EQ(session.captureHealth()["deliveredFrames"].toLongLong(), 0);
  EXPECT_FALSE(session.captureHealth().contains("averageEncodeMs"));
  EXPECT_FALSE(session.captureHealth().contains("transport"));
  session.stop();
}

TEST(RecordingSessionHealthTest, RatesTransportAndQueueUseObservedCounters) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  RecordingSession session("rates.mkv", &capture, &encoder, &muxer,
                           nullptr, false, 128, 0);
  auto now = std::make_shared<std::atomic<int64_t>>(5000000);
  RecordingSessionTestPeer::clock(session, now);
  ASSERT_TRUE(session.start());
  EXPECT_FALSE(session.captureHealth().contains("actualCaptureFps"));
  EXPECT_EQ(session.captureHealth()["queueDepth"].toInt(), 0);
  EXPECT_EQ(session.captureHealth()["queueCapacity"].toInt(), 30);
  EXPECT_TRUE(session.captureHealth()["audioStreams"].toList().isEmpty());
  for (int i = 0; i < 10; ++i)
    RecordingSessionTestPeer::delivered(session, true, 1280, 720);
  EXPECT_EQ(session.captureHealth()["transport"].toString(), "dma-buf");
  EXPECT_EQ(session.captureHealth()["transportChanges"].toInt(), 0);
  ASSERT_TRUE(RecordingSessionTestPeer::output(session, 0, 2000));
  ASSERT_TRUE(RecordingSessionTestPeer::output(session, 1, 1000));
  now->fetch_add(2000000);
  const auto active = session.captureHealth();
  EXPECT_DOUBLE_EQ(active["actualCaptureFps"].toDouble(), 5.0);
  EXPECT_DOUBLE_EQ(active["actualOutputKbps"].toDouble(), 12.0);
  EXPECT_DOUBLE_EQ(active["rateWindowSeconds"].toDouble(), 2.0);
  EXPECT_DOUBLE_EQ(active["sessionSeconds"].toDouble(), 2.0);
  // A generic encoder's packet/no-packet result cannot prove acceptance.
  EXPECT_FALSE(active.contains("actualEncodeFps"));
  EXPECT_EQ(session.captureHealth()["actualCaptureFps"], active["actualCaptureFps"]);
  RecordingSessionTestPeer::delivered(session, false, 640, 360);
  auto fallback = session.captureHealth();
  EXPECT_EQ(fallback["transport"].toString(), "shm");
  EXPECT_EQ(fallback["shmTransitions"].toInt(), 1);
  EXPECT_EQ(fallback["inputWidth"].toInt(), 640);
  RecordingSessionTestPeer::delivered(session, false, 640, 360);
  EXPECT_EQ(session.captureHealth()["transportChanges"].toInt(), 1);
  RecordingSessionTestPeer::delivered(session, true, 1280, 720);
  EXPECT_EQ(session.captureHealth()["transportChanges"].toInt(), 2);
  now->fetch_add(1000000);
  EXPECT_DOUBLE_EQ(session.captureHealth()["actualCaptureFps"].toDouble(), 3.0);
  now->fetch_add(1000000);
  auto stalled = session.captureHealth();
  EXPECT_DOUBLE_EQ(stalled["actualCaptureFps"].toDouble(), 0.0);
  EXPECT_DOUBLE_EQ(stalled["actualOutputKbps"].toDouble(), 0.0);
  now->fetch_add(6000000); // returning to the Dashboard after a long absence
  EXPECT_FALSE(session.captureHealth().contains("actualCaptureFps"));
  now->fetch_add(1000000);
  EXPECT_DOUBLE_EQ(session.captureHealth()["actualCaptureFps"].toDouble(), 0.0);
  session.stop();
  ASSERT_TRUE(session.start());
  EXPECT_FALSE(session.captureHealth().contains("actualCaptureFps"));
  EXPECT_EQ(session.captureHealth()["transportChanges"].toInt(), 0);
  session.stop();
}

TEST(RecordingSessionDiagnosticsTest, RecentTimingPercentilesExpireAndStayBounded) {
  EncodeTimingWindow timings;
  const int64_t now = 10000000;
  EXPECT_EQ(timings.summary(now).count, 0);
  for (int i = 1; i <= 100; ++i) timings.record(now, i * 1000);
  const auto summary = timings.summary(now);
  EXPECT_EQ(summary.count, 100);
  EXPECT_DOUBLE_EQ(summary.averageMs, 50.5);
  EXPECT_DOUBLE_EQ(summary.maximumMs, 100.0);
  EXPECT_DOUBLE_EQ(summary.p95Ms, 95.0);
  EXPECT_EQ(timings.summary(now + EncodeTimingWindow::windowUs).count, 0);
  EXPECT_EQ(timings.summary(now - 1).count, 0);
  for (size_t i = 0; i < EncodeTimingWindow::capacity + 20; ++i)
    timings.record(now + 1, 2000);
  EXPECT_EQ(timings.summary(now + 1).count, EncodeTimingWindow::capacity);
  EXPECT_DOUBLE_EQ(timings.summary(now + 1).p95Ms, 2.0);
  timings.clear();
  EXPECT_EQ(timings.summary(now + 1).count, 0);
}

TEST(RecordingSessionDiagnosticsTest, TimingPublicationSupportsConcurrentSnapshots) {
  EncodeTimingWindow timings;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) timings.record(10000000, i);
    done = true;
  });
  do {
    const auto summary = timings.summary(10000000);
    EXPECT_LE(summary.count, EncodeTimingWindow::capacity);
    EXPECT_LE(summary.averageMs, summary.maximumMs);
    EXPECT_LE(summary.p95Ms, summary.maximumMs);
  } while (!done);
  writer.join();
  EXPECT_EQ(timings.summary(10000000).count, EncodeTimingWindow::capacity);
}

TEST(RecordingSessionHealthTest, EncodedRateIncludesAcceptedInputsBeforePacketOutput) {
  if (!avcodec_find_encoder_by_name("libx264")) GTEST_SKIP() << "x264 unavailable";
  FFmpegEncoder encoder(IEncoder::EncoderType::Software_x264, "libx264");
  EncoderConfig config;
  config.width = config.height = 64;
  ASSERT_TRUE(encoder.initialize(config));
  FakeCapture capture;
  FakeMuxer muxer;
  RecordingSession session("encoded-rate.mkv", &capture, &encoder, &muxer,
                           nullptr, false, 128, 0);
  auto now = std::make_shared<std::atomic<int64_t>>(5000000);
  RecordingSessionTestPeer::clock(session, now);
  ASSERT_TRUE(session.start());
  std::vector<uint8_t> pixels(64 * 64 * 4, 80);
  VideoFrame frame{};
  frame.data = pixels.data(); frame.width = frame.height = 64;
  frame.stride = 64 * 4; frame.pixelFormat = AV_PIX_FMT_RGBA;
  capture.fire(frame);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!encoder.acceptedFrames() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(encoder.acceptedFrames(), 1);
  now->fetch_add(1000000);
  const auto health = session.captureHealth();
  EXPECT_DOUBLE_EQ(health["actualCaptureFps"].toDouble(), 1.0);
  EXPECT_DOUBLE_EQ(health["actualEncodeFps"].toDouble(), 1.0);
  EXPECT_EQ(health["acceptedFrames"].toLongLong(), 1);
  session.stop();
  ASSERT_TRUE(encoder.initialize(config));
  EXPECT_EQ(encoder.acceptedFrames(), 0);
}

TEST(RecordingSessionHealthTest, AudioFormatsAndSourceAreBoundPipelineData) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC)) GTEST_SKIP() << "AAC unavailable";
  class NamedCapture : public FakeCapture {
    QString sourceLabel() const override { return "Captured window"; }
    QString sourceProcessName() const override { return "org.example.Game"; }
    uint32_t sourcePid() const override { return 42; }
  } capture;
  for (bool separate : {true, false}) {
    AudioMixer mixer(44100, 1);
    FakeVideoEncoder encoder;
    FakeMuxer muxer;
    RecordingSession session("audio-format.mkv", &capture, &encoder, &muxer,
                             &mixer, separate, 128, 0);
    ASSERT_TRUE(session.start());
    const auto health = session.captureHealth();
    const auto streams = health["audioStreams"].toList();
    ASSERT_EQ(streams.size(), separate ? 3 : 1);
    for (const auto &stream : streams) {
      const auto info = stream.toMap();
      EXPECT_EQ(info["codec"].toString(), "AAC");
      EXPECT_EQ(info["sampleRate"].toInt(), 44100);
      EXPECT_EQ(info["channels"].toInt(), 1);
      EXPECT_EQ(info["bitrateKbps"].toInt(), 128);
      EXPECT_GT(info["stream"].toInt(), 0);
    }
    EXPECT_EQ(health["sourceLabel"].toString(), "Captured window");
    EXPECT_EQ(health["sourceAppId"].toString(), "org.example.Game");
    EXPECT_EQ(health["sourcePid"].toInt(), 42);
    EXPECT_EQ(health["separateAudio"].toBool(), separate);
    session.stop();
  }
}

class FakeAudioSource : public IAudioSource {
public:
  ~FakeAudioSource() override {
    if (destroyed)
      *destroyed = true;
  }
  bool *destroyed = nullptr;
  bool startSucceeds = true;
  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback cb) override {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_cb = std::move(cb);
  }
  bool start() override {
    ++m_starts;
    m_capturing = startSucceeds;
    return startSucceeds;
  }
  void stop() override { m_capturing = false; }
  bool isCapturing() const override { return m_capturing; }
  SourceType sourceType() const override { return SourceType::DesktopAudio; }
  QString displayName() const override { return QStringLiteral("fake"); }
  QString identifier() const override { return QStringLiteral("fake"); }
  float peakLevel() const override { return 0.0f; }
  void setVolume(float) override {}
  float volume() const override { return 1.0f; }

  int starts() const { return m_starts; }

  void fire(const float *samples, int frames) {
    AudioCallback callback;
    {
      std::lock_guard<std::mutex> lock(m_callbackMutex);
      callback = m_cb;
    }
    if (!callback)
      return;
    AudioFrame f{};
    f.samples = samples;
    f.frameCount = frames;
    f.channels = 2;
    f.sampleRate = 48000;
    f.timestamp = 0;
    callback(f);
  }

private:
  AudioCallback m_cb;
  std::mutex m_callbackMutex;
  std::atomic<bool> m_capturing{false};
  std::atomic<int> m_starts{0};
};

TEST(RecordingSessionHealthTest, HistoryKeepsFallbackAndLaterSeparateVoice) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC)) GTEST_SKIP() << "AAC unavailable";
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  mixer.addSource(std::make_unique<FakeAudioSource>(), 0);
  RecordingSession session("unused-history.mkv", &capture, &encoder, &muxer, &mixer, true, 128, 3);
  session.setGameIsFallback(true);
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(settleMixerSources(mixer));
  VideoFrame video{}; video.timestamp = 0;
  capture.fire(video);
  auto history = AudioRoutingInfo::fromJson(session.audioHistory());
  ASSERT_TRUE(history.known);
  EXPECT_TRUE(history.system); EXPECT_FALSE(history.game); EXPECT_FALSE(history.voice);
  EXPECT_EQ(history.primary, 0); EXPECT_EQ(history.voiceTrack, 1); EXPECT_EQ(history.micTrack, 2);
  session.setGameIsFallback(false);
  auto voice = std::make_unique<FakeAudioSource>();
  auto *source = voice.get();
  mixer.addSource(std::move(voice), 1);
  std::vector<float> samples(4096, 0.0f); // Silence still counts as separate source delivery.
  source->fire(samples.data(), 2048);
  session.stop();
  history = AudioRoutingInfo::fromJson(session.audioHistory());
  EXPECT_TRUE(history.system); EXPECT_TRUE(history.game); EXPECT_TRUE(history.voice);
}

// The encoding worker is parked at a controlled video encode boundary while
// tests advance the production clock driver directly. No scheduling sleeps or
// real devices: clock, PCM, and capture timestamps are explicit inputs.
struct MergedClockScenario {
  std::shared_ptr<std::atomic<int64_t>> now = std::make_shared<std::atomic<int64_t>>(0);
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer{48000, 2};
  std::unique_ptr<RecordingSession> session;
  FakeAudioSource *source = nullptr;
  std::shared_ptr<std::promise<void>> entered = std::make_shared<std::promise<void>>();
  std::promise<void> release;
  bool released = false;
  bool start() {
    muxer.collectAudio = true;
    auto audio = std::make_unique<FakeAudioSource>();
    source = audio.get();
    mixer.addSource(std::move(audio), 0);
    session = std::make_unique<RecordingSession>(
        "unused-clock.mkv", &capture, &encoder, &muxer, &mixer, false, 128, 1);
    RecordingSessionTestPeer::clock(*session, now);
    auto ready = entered->get_future();
    auto releasedFuture = release.get_future().share();
    auto first = std::make_shared<std::atomic<bool>>(true);
    encoder.encodeGate = [entered = entered, releasedFuture, first] {
      if (first->exchange(false)) {
        entered->set_value();
        releasedFuture.wait_for(std::chrono::seconds(10));
      }
    };
    if (!session->start() || !settleMixerSources(mixer)) return false;
    RecordingSessionTestPeer::disableDrift(*session);
    std::vector<uint8_t> bytes(64 * 64 * 4, 0x80);
    VideoFrame frame{};
    frame.data = bytes.data(); frame.width = 64; frame.height = 64;
    frame.stride = 64 * 4; frame.pixelFormat = AV_PIX_FMT_BGRA; frame.timestamp = 0;
    capture.fire(frame);
    return ready.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  }
  void advance(int64_t us) {
    *now = us;
    RecordingSessionTestPeer::videoTime(*session, us);
  }
  void catchUp(bool final = false) {
    for (int step = 0; step < 10000; ++step)
      if (!RecordingSessionTestPeer::tick(*session, final)) return;
    ADD_FAILURE() << "Clock did not settle its bounded catch-up";
  }
  void finish() {
    if (!released) { release.set_value(); released = true; }
    if (session) session->stop();
  }
  ~MergedClockScenario() { finish(); }
};

VideoFrame makeSoftwareFrame(std::vector<uint8_t> &storage) {
  storage.assign(64 * 64 * 4, 0x80);
  VideoFrame frame{};
  frame.data = storage.data();
  frame.width = 64;
  frame.height = 64;
  frame.stride = 64 * 4;
  frame.timestamp = 0;
  frame.pixelFormat = AV_PIX_FMT_BGRA;
  return frame;
}

bool decodedChannelEnergy(const FakeMuxer &muxer, std::array<double, 2> &energy) {
  auto *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  AVFrame *frame = av_frame_alloc();
  AVPacket *packet = av_packet_alloc();
  bool ok = codec && context && frame && packet;
  if (ok) {
    context->extradata = static_cast<uint8_t *>(av_mallocz(
        muxer.audioExtraData.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    ok = context->extradata != nullptr;
  }
  if (ok) {
    context->extradata_size = muxer.audioExtraData.size();
    std::memcpy(context->extradata, muxer.audioExtraData.data(),
                muxer.audioExtraData.size());
    ok = avcodec_open2(context, codec, nullptr) >= 0;
  }
  for (const auto &payload : muxer.audioPayloads) {
    if (!ok)
      break;
    ok = av_new_packet(packet, payload.size()) >= 0;
    if (!ok)
      break;
    std::memcpy(packet->data, payload.data(), payload.size());
    ok = avcodec_send_packet(context, packet) >= 0;
    av_packet_unref(packet);
    while (ok) {
      const int result = avcodec_receive_frame(context, frame);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        break;
      ok = result >= 0 && frame->format == AV_SAMPLE_FMT_FLTP &&
           frame->ch_layout.nb_channels == 2;
      if (!ok)
        break;
      for (int channel = 0; channel < 2; ++channel) {
        const auto *samples = reinterpret_cast<const float *>(frame->data[channel]);
        for (int i = 0; i < frame->nb_samples; ++i)
          energy[channel] += samples[i] * samples[i];
      }
      av_frame_unref(frame);
    }
  }
  av_packet_free(&packet);
  av_frame_free(&frame);
  avcodec_free_context(&context);
  return ok;
}

} // namespace

TEST(RecordingSessionTest, AudioGateOpensOnFirstFrameWithoutDispatcher) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";

  FakeCapture capture;
  FakeVideoEncoder videoEncoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2, nullptr);

  auto source = std::make_unique<FakeAudioSource>();
  FakeAudioSource *rawSource = source.get();
  mixer.addSource(std::move(source), 0);

  // No setFrameDispatcher call: this is the DVR segment configuration.
  RecordingSession session(QStringLiteral("/tmp/trophy-test-session.mkv"),
                           &capture, &videoEncoder, &muxer, &mixer,
                           true /*separateTracks*/, 128, 3, {});
  ASSERT_TRUE(session.start());

  std::vector<float> sine(1024 * 2);
  for (int i = 0; i < 1024; ++i) {
    const float v =
        0.2f * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
    sine[i * 2] = v;
    sine[i * 2 + 1] = v;
  }

  // Pre-video audio is unpairable head content: still dropped.
  for (int i = 0; i < 5; ++i)
    rawSource->fire(sine.data(), 1024);
  EXPECT_EQ(muxer.audioPackets(), 0);

  // First video frame latches the A/V origin even without a dispatcher.
  std::vector<uint8_t> storage;
  const VideoFrame frame = makeSoftwareFrame(storage);
  capture.fire(frame);

  // Audio past the first frame must mux immediately — not after the 10s
  // gate timeout (the DVR bug dropped a full 10s per segment here).
  for (int i = 0; i < 10; ++i)
    rawSource->fire(sine.data(), 1024);
  EXPECT_GT(muxer.audioPackets(), 0);

  session.stop(false);
}

TEST(RecordingSessionTest, MergedOutageClockBoundsCatchupAndDoesNotReplayResumeBuffers) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  MergedClockScenario graph;
  ASSERT_TRUE(graph.start());
  const std::vector<float> input(2048, 0.25f);
  graph.source->fire(input.data(), 1024);
  graph.source->fire(input.data(), 1024);
  const auto before = RecordingSessionTestPeer::submitted(*graph.session);
  graph.advance(2000000);
  EXPECT_TRUE(RecordingSessionTestPeer::tick(*graph.session));
  EXPECT_TRUE(RecordingSessionTestPeer::clocked(*graph.session));
  EXPECT_LE(RecordingSessionTestPeer::submitted(*graph.session) - before, 8192);
  graph.catchUp();
  const auto filled = RecordingSessionTestPeer::submitted(*graph.session);
  EXPECT_GT(filled, 90000);
  EXPECT_LT(filled, 97000);
  for (int burst = 0; burst < 50; ++burst)
    graph.source->fire(input.data(), 1024);
  graph.catchUp();
  EXPECT_EQ(RecordingSessionTestPeer::submitted(*graph.session), filled);
  EXPECT_LE(RecordingSessionTestPeer::accumulatedSamples(*graph.session, 0), 8192u * 2);
  const std::vector<float> backlog(100000 * 2, 0.5f);
  graph.source->fire(backlog.data(), 100000);
  EXPECT_LE(RecordingSessionTestPeer::accumulatedSamples(*graph.session, 0), 8192u * 2);
  RecordingSessionTestPeer::videoTime(*graph.session, INT64_MAX);
  RecordingSessionTestPeer::videoTime(*graph.session, -1);
  graph.catchUp();
  EXPECT_EQ(RecordingSessionTestPeer::submitted(*graph.session), filled);
  graph.finish();
  EXPECT_TRUE(graph.session->outputSucceeded());
}

TEST(RecordingSessionTest, MergedSilenceClockRequiresFirstVideo) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  auto audio = std::make_unique<FakeAudioSource>();
  auto *source = audio.get();
  mixer.addSource(std::move(audio), 0);
  RecordingSession session("unused-before-video.mkv", &capture, &encoder, &muxer,
                           &mixer, false, 128, 1);
  auto now = std::make_shared<std::atomic<int64_t>>(0);
  RecordingSessionTestPeer::clock(session, now);
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(settleMixerSources(mixer));
  *now = 1000000;
  const std::vector<float> input(2048, 0.2f);
  source->fire(input.data(), 1024);
  EXPECT_FALSE(RecordingSessionTestPeer::tick(session));
  EXPECT_EQ(RecordingSessionTestPeer::submitted(session), 0);
  EXPECT_EQ(muxer.audioPackets(), 0);
  session.stop();
}

TEST(RecordingSessionTest, MergedOutageClockKeepsLateAudioOriginAndRejectsRetiredTick) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  MergedClockScenario graph;
  ASSERT_TRUE(graph.start());
  graph.advance(500000);
  const std::vector<float> input(2048, 0.25f);
  graph.source->fire(input.data(), 1024);
  graph.advance(2000000);
  graph.catchUp();
  graph.finish();
  const auto pts = graph.muxer.audioPts();
  ASSERT_FALSE(pts.empty());
  EXPECT_GE(pts.front(), 470); // Existing 500ms start offset, minus AAC priming.
  EXPECT_LE(pts.front(), 500);
  const auto before = RecordingSessionTestPeer::submitted(*graph.session);
  graph.advance(10000000);
  EXPECT_FALSE(RecordingSessionTestPeer::tick(*graph.session, true));
  EXPECT_EQ(RecordingSessionTestPeer::submitted(*graph.session), before);
}

TEST(RecordingSessionTest, EncodingWorkerAdvancesMergedSilenceWithoutInputCallbacks) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  MergedClockScenario graph;
  ASSERT_TRUE(graph.start());
  graph.advance(2000000);
  graph.release.set_value();
  graph.released = true;
  ASSERT_TRUE(graph.muxer.waitForAudioPackets(80)); // Native worker rendezvous, no sleeps.
  graph.finish();
  EXPECT_GT(RecordingSessionTestPeer::submitted(*graph.session), 96000);
  EXPECT_TRUE(graph.session->outputSucceeded());
}

TEST(RecordingSessionTest, MergedOutageProducesRealAacSilenceAndResumeAtUncompressedPts) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  MergedClockScenario graph;
  ASSERT_TRUE(graph.start());
  const auto tone = [](int64_t start) {
    std::vector<float> samples(2048);
    for (int frame = 0; frame < 1024; ++frame) {
      const float value = 0.25f * std::sin(2.0 * 3.141592653589793 * 440.0 *
                                        (start + frame) / 48000.0);
      samples[frame * 2] = samples[frame * 2 + 1] = value;
    }
    return samples;
  };
  for (int block = 0; block < 12; ++block) {
    graph.advance(block * 1024LL * 1000000 / 48000);
    const auto samples = tone(block * 1024LL);
    graph.source->fire(samples.data(), 1024);
  }
  graph.advance(2000000);
  graph.catchUp();
  for (int block = 0; block < 20; ++block) {
    graph.advance(2000000 + (block + 1) * 1024LL * 1000000 / 48000);
    const auto samples = tone(96000 + block * 1024LL);
    graph.source->fire(samples.data(), 1024);
    graph.catchUp();
  }
  graph.finish();
  const auto pts = graph.muxer.audioPts();
  ASSERT_EQ(pts.size(), graph.muxer.audioPayloads.size());
  ASSERT_GT(pts.size(), 100u);
  for (size_t index = 1; index < pts.size(); ++index) EXPECT_GT(pts[index], pts[index - 1]);
  EXPECT_GT(pts.back(), 2350);
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
  ASSERT_NE(codec, nullptr);
  AVCodecContext *context = avcodec_alloc_context3(codec);
  ASSERT_NE(context, nullptr);
  context->pkt_timebase = {1, 1000};
  context->extradata_size = static_cast<int>(graph.muxer.audioExtraData.size());
  context->extradata = static_cast<uint8_t *>(av_mallocz(context->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
  ASSERT_NE(context->extradata, nullptr);
  std::memcpy(context->extradata, graph.muxer.audioExtraData.data(), context->extradata_size);
  ASSERT_GE(avcodec_open2(context, codec, nullptr), 0);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  ASSERT_NE(packet, nullptr);
  ASSERT_NE(frame, nullptr);
  int silentFrames = 0, restoredFrames = 0;
  int64_t firstRestoredPts = INT64_MAX;
  for (size_t index = 0; index < pts.size(); ++index) {
    const auto &payload = graph.muxer.audioPayloads[index];
    ASSERT_GE(av_new_packet(packet, static_cast<int>(payload.size())), 0);
    std::memcpy(packet->data, payload.data(), payload.size());
    packet->pts = packet->dts = pts[index];
    ASSERT_GE(avcodec_send_packet(context, packet), 0);
    av_packet_unref(packet);
    while (avcodec_receive_frame(context, frame) == 0) {
      ASSERT_EQ(frame->format, AV_SAMPLE_FMT_FLTP);
      double energy = 0;
      const auto *samples = reinterpret_cast<const float *>(frame->data[0]);
      for (int sample = 0; sample < frame->nb_samples; ++sample) energy += samples[sample] * samples[sample];
      energy /= frame->nb_samples;
      if (frame->pts >= 500 && frame->pts <= 1800) {
        EXPECT_LT(energy, 0.000001);
        ++silentFrames;
      }
      if (frame->pts > 1500 && energy > 0.001) {
        firstRestoredPts = std::min(firstRestoredPts, frame->pts);
        ++restoredFrames;
      }
      av_frame_unref(frame);
    }
  }
  EXPECT_GT(silentFrames, 40);
  EXPECT_GT(restoredFrames, 10);
  EXPECT_GE(firstRestoredPts, 1900);
  EXPECT_LE(firstRestoredPts, 2080);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
}

TEST(RecordingSessionTest, FailedCaptureStartUnpublishesSessionFrameCallback) {
  FakeCapture capture;
  capture.startSucceeds = false;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  {
    RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                             nullptr, true, 128, 0, {});
    EXPECT_FALSE(session.start());
    EXPECT_FALSE(capture.hasCallback());
  }
  // A capture implementation may still own native resources after a failed
  // start. No dangling [this] callback may remain after session destruction.
  EXPECT_FALSE(capture.hasCallback());
}

TEST(RecordingSessionTest, PacketAndTrailerFailuresRemainLatchedAfterDrain) {
  for (const int failure : {0, 1, 2, 3}) {
    SCOPED_TRACE(failure);
    FakeCapture capture;
    FakeVideoEncoder encoder;
    encoder.emitPackets = failure != 2;
    encoder.emitFlushPacket = failure == 2;
    FakeMuxer muxer;
    muxer.writeSucceeds = failure != 1 && failure != 2;
    muxer.finalizeSucceeds = failure != 3;
    RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                             nullptr, true, 128, 0);
    QSignalSpy errors(&session, &RecordingSession::errorOccurred);
    ASSERT_TRUE(session.start());
    EXPECT_FALSE(session.outputSucceeded());
    std::vector<uint8_t> storage;
    capture.fire(makeSoftwareFrame(storage));
    session.stop(); // joins video writes and flush before inspecting output
    EXPECT_EQ(session.outputFailed(), failure != 0);
    EXPECT_EQ(session.outputSucceeded(), failure == 0);
    EXPECT_EQ(muxer.finalizations(), 1);
    // No callback/flush/finalize signal can reenter the manager's teardown.
    EXPECT_EQ(errors.count(), 0);
    session.stop();
    EXPECT_EQ(session.outputSucceeded(), failure == 0);
    EXPECT_EQ(muxer.finalizations(), 1);
  }
}

TEST(RecordingSessionTest, SeparateAndMergedAudioWriteFailuresFailOutput) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  for (const bool separate : {false, true}) {
    SCOPED_TRACE(separate);
    FakeCapture capture;
    FakeVideoEncoder encoder;
    FakeMuxer muxer;
    muxer.writeSucceeds = false;
    AudioMixer mixer(48000, 2);
    auto source = std::make_unique<FakeAudioSource>();
    auto *rawSource = source.get();
    mixer.addSource(std::move(source), 0);
    RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                             &mixer, separate, 128, 1);
    ASSERT_TRUE(session.start());
    std::vector<uint8_t> storage;
    capture.fire(makeSoftwareFrame(storage));
    std::vector<float> samples(2048, 0.1f);
    for (int i = 0; i < 8; ++i)
      rawSource->fire(samples.data(), 1024);
    session.stop();
    ASSERT_GT(muxer.audioPackets(), 0);
    EXPECT_TRUE(session.outputFailed());
    EXPECT_FALSE(session.outputSucceeded());
  }
}

TEST(RecordingSessionTest, MergedMutedTrackKeepsOtherTracksAdvancing) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  auto first = std::make_unique<FakeAudioSource>();
  auto second = std::make_unique<FakeAudioSource>();
  auto *game = first.get();
  auto *mic = second.get();
  mixer.addSource(std::move(first), 0);
  mixer.addSource(std::move(second), 1);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, false, 128, 2, {});
  ASSERT_TRUE(session.start());
  std::vector<uint8_t> video;
  capture.fire(makeSoftwareFrame(video));
  std::vector<float> audio(2048, 0.1f);
  for (int i = 0; i < 8; ++i) {
    game->fire(audio.data(), 1024);
    mic->fire(audio.data(), 1024);
  }
  const int before = muxer.audioPackets();
  ASSERT_GT(before, 0);
  mixer.setTrackMuted(1, true);
  for (int i = 0; i < 100; ++i) {
    game->fire(audio.data(), 1024);
    mic->fire(audio.data(), 1024);
  }
  EXPECT_GT(muxer.audioPackets(), before);
  EXPECT_LT(RecordingSessionTestPeer::accumulatedSamples(session, 0), 8192u);
  session.stop();
}

TEST(RecordingSessionTest, TerminalSourceErrorRebindsUnchangedNodeSerial) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  bool oldDestroyed = false;
  auto source = std::make_unique<FakeAudioSource>();
  auto *old = source.get();
  old->destroyed = &oldDestroyed;
  mixer.addSource(std::move(source), 0);
  // Fake an active source without starting the mixer: its replacement is
  // merely slotted, never initialized against the host PipeWire daemon.
  ASSERT_TRUE(old->start());
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 3, {});
  RecordingSessionTestPeer::bindKnownNode(session);
  RecordingSessionTestPeer::deliverSameNode(session);
  EXPECT_FALSE(oldDestroyed); // same healthy source must not be restarted
  old->stop(); // terminal listener errors likewise clear isCapturing
  RecordingSessionTestPeer::deliverSameNode(session);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (mixer.sourceOperationPending(0) && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(oldDestroyed); // node still exists, but the capture died
}

TEST(RecordingSessionTest, MissingVoiceRetiresSourceEvenAfterDisconnectedFlagCleared) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  bool retired = false, gameRetired = false;
  auto voice = std::make_unique<FakeAudioSource>();
  voice->destroyed = &retired;
  mixer.addSource(std::move(voice), 1);
  auto game = std::make_unique<FakeAudioSource>();
  game->destroyed = &gameRetired;
  mixer.addSource(std::move(game), 0);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 3, {});
  session.markSourceConnected(1);
  session.forgetTrack(1); // Old bug only forgot flags and retained native source.
  RecordingSessionTestPeer::missingNode(session, 1, false);
  RecordingSessionTestPeer::missingNode(session, 0, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (mixer.sourceOperationPending(1) && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(retired);
  EXPECT_FALSE(gameRetired); // No removal of the Game/desktop fallback leg.
}

TEST(RecordingSessionTest, MissingMergedSourceCannotStallOrGrowOtherTrack) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";
  for (int missing : {0, 1}) {
    SCOPED_TRACE(missing);
    FakeCapture capture;
    FakeVideoEncoder encoder;
    FakeMuxer muxer;
    AudioMixer mixer(48000, 2);
    std::array<FakeAudioSource *, 2> sources;
    for (int i = 0; i < 2; ++i) {
      auto source = std::make_unique<FakeAudioSource>();
      sources[i] = source.get();
      mixer.addSource(std::move(source), i);
    }
    RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                             &mixer, false, 128, 2, {});
    ASSERT_TRUE(session.start());
    std::vector<uint8_t> video;
    capture.fire(makeSoftwareFrame(video));
    std::vector<float> audio(2048, 0.1f);
    // Missing from the start: a healthy but silent PipeWire stream is still
    // isCapturing=true, so this cannot rely on capture-state alone.
    for (int i = 0; i < 100; ++i)
      sources[1 - missing]->fire(audio.data(), 1024);
    EXPECT_GT(muxer.audioPackets(), 80);
    EXPECT_LE(RecordingSessionTestPeer::accumulatedSamples(session, 1 - missing),
              12000u);
    const int beforeReturn = muxer.audioPackets();
    for (int i = 0; i < 20; ++i) {
      sources[0]->fire(audio.data(), 1024);
      sources[1]->fire(audio.data(), 1024);
    }
    EXPECT_GT(muxer.audioPackets(), beforeReturn);
    // Then vanishes after contributing: old content must not accumulate
    // indefinitely or burst out when the missing source next returns.
    const int beforeLoss = muxer.audioPackets();
    for (int i = 0; i < 100; ++i)
      sources[1 - missing]->fire(audio.data(), 1024);
    EXPECT_GT(muxer.audioPackets(), beforeLoss + 80);
    EXPECT_LE(RecordingSessionTestPeer::accumulatedSamples(session, 1 - missing),
              12000u);
    const int beforeStop = muxer.audioPackets();
    session.stop();
    EXPECT_GT(muxer.audioPackets(), beforeStop + 2);
    EXPECT_EQ(RecordingSessionTestPeer::accumulatedSamples(session, 0), 0u);
    EXPECT_EQ(RecordingSessionTestPeer::accumulatedSamples(session, 1), 0u);
  }
}

TEST(RecordingSessionTest, IdleChecksNeverRestartFailedDesktopOrMicrophone) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  auto source = std::make_unique<FakeAudioSource>();
  auto *raw = source.get();
  mixer.addSource(std::move(source), 1);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 2, {});
  ASSERT_TRUE(session.start());
  const auto settle = [&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (mixer.sourceOperationPending(1) && std::chrono::steady_clock::now() < deadline) {
      QCoreApplication::processEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(mixer.sourceOperationPending(1));
  };
  settle();
  EXPECT_EQ(raw->starts(), 1);
  raw->stop();
  raw->startSucceeds = false;
  RecordingSessionTestPeer::checkNodes(session);
  settle();
  EXPECT_EQ(raw->starts(), 2);
  EXPECT_FALSE(raw->isCapturing());
  raw->startSucceeds = true;
  RecordingSessionTestPeer::checkNodes(session);
  settle();
  EXPECT_EQ(raw->starts(), 3);
  EXPECT_TRUE(raw->isCapturing());
  session.stop();
  RecordingSessionTestPeer::checkNodes(session);
  EXPECT_EQ(raw->starts(), 3);
  EXPECT_FALSE(raw->isCapturing());
}

TEST(RecordingSessionTest, VoiceBindSuppressedWhileGameTrackIsFullMixFallback) {
  // Manual recordings leave track 0 on the whole-system monitor, which
  // already carries voice: binding a separate voice leg records every
  // call twice (once per track). The retry must not hunt voice while
  // fallback holds, and must resume once the game track upgrades to a
  // real app node.
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 3, {});
  int calls = 0;
  RecordingSessionTestPeer::setLookup(session, [&] { ++calls; return 23u; });
  session.setVoiceChatApp("Discord");
  session.setVoiceTrack(1);
  session.setGameIsFallback(true);
  ASSERT_TRUE(session.start());
  RecordingSessionTestPeer::checkNodes(session);
  EXPECT_FALSE(RecordingSessionTestPeer::lookupPendingTrack(session, 1));
  EXPECT_EQ(calls, 0);
  session.setGameIsFallback(false);
  RecordingSessionTestPeer::checkNodes(session);
  EXPECT_TRUE(RecordingSessionTestPeer::lookupPendingTrack(session, 1));
  session.stop();
}

TEST(RecordingSessionTest, HeldLookupKeepsHeartbeatAndCannotAdoptOldIdentity) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 0, {});
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  std::atomic<int> calls{0};
  RecordingSessionTestPeer::setLookup(session, [&] {
    ++calls;
    entered.set_value();
    released.wait_for(std::chrono::seconds(3));
    return 23u;
  });
  ASSERT_TRUE(session.start());
  RecordingSessionTestPeer::requestLookup(session);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  RecordingSessionTestPeer::requestLookup(session);
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_EQ(calls, 1);
  session.setGameAudioHints(18, {"replacement"});
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (RecordingSessionTestPeer::lookupPending(session) &&
         std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(RecordingSessionTestPeer::lookupPending(session));
  EXPECT_FALSE(RecordingSessionTestPeer::connected(session));
  EXPECT_FALSE(mixer.sourceOperationPending(0));
  session.stop();
}

TEST(RecordingSessionTest, StoppedSessionRejectsHeldLookupWithoutJoiningOnStop) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, true, 128, 0, {});
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  RecordingSessionTestPeer::setLookup(session, [&] {
    entered.set_value();
    released.wait_for(std::chrono::seconds(3));
    return 23u;
  });
  ASSERT_TRUE(session.start());
  RecordingSessionTestPeer::requestLookup(session);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  session.stop();
  EXPECT_TRUE(RecordingSessionTestPeer::lookupPending(session));
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (RecordingSessionTestPeer::lookupPending(session) &&
         std::chrono::steady_clock::now() < deadline) {
    RecordingSessionTestPeer::deliverLookups(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(RecordingSessionTestPeer::lookupPending(session));
  EXPECT_FALSE(RecordingSessionTestPeer::connected(session));
  EXPECT_FALSE(mixer.sourceOperationPending(0));
}

TEST(RecordingSessionTest, DestroyedSessionTransfersHeldLookupWithoutBlockingRotation) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  auto session = std::make_unique<RecordingSession>(
      "unused.mkv", &capture, &encoder, &muxer, &mixer, true, 128, 0);
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  RecordingSessionTestPeer::setLookup(*session, [&] {
    entered.set_value();
    released.wait_for(std::chrono::seconds(3));
    return 23u;
  });
  ASSERT_TRUE(session->start());
  RecordingSessionTestPeer::requestLookup(*session);
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  session.reset();
  EXPECT_TRUE(mixer.discardedNodeLookupPending(0));
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  RecordingSession successor("unused-2.mkv", &capture, &encoder, &muxer,
                             &mixer, true, 128, 0);
  std::atomic<int> successorCalls{0};
  RecordingSessionTestPeer::setLookup(successor, [&] {
    ++successorCalls;
    return 0xffffffffu;
  });
  RecordingSessionTestPeer::requestLookup(successor);
  EXPECT_EQ(successorCalls, 0); // Query slot remains coalesced across handoff.
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (mixer.discardedNodeLookupPending(0) &&
         std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(mixer.discardedNodeLookupPending(0));
  EXPECT_FALSE(mixer.sourceOperationPending(0));
  EXPECT_FALSE(RecordingSessionTestPeer::connected(successor));
}

TEST(RecordingSessionTest, MergeGraceDoesNotShiftOriginAndLargeInputsStayBounded) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  AudioMixer mixer(48000, 2);
  auto source = std::make_unique<FakeAudioSource>();
  auto *raw = source.get();
  mixer.addSource(std::move(source), 0);
  RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                           &mixer, false, 128, 2, {});
  ASSERT_TRUE(session.start());
  // Controlled timestamps: video began 500ms ago, first retained audio 50ms
  // after it. Encoding it now must not add those 450ms of queueing latency.
  RecordingSessionTestPeer::setMergeOrigin(session);
  std::vector<float> audio(64 * 1024 * 2, 0.1f);
  raw->fire(audio.data(), 64 * 1024);
  EXPECT_GT(muxer.audioPackets(), 50);
  EXPECT_LE(RecordingSessionTestPeer::accumulatedSamples(session, 0), 12000u);
  const auto pts = muxer.audioPts();
  ASSERT_FALSE(pts.empty());
  // AAC delay is ~21ms: a 50ms origin produces a head packet around 29ms.
  EXPECT_GE(pts.front(), 20);
  EXPECT_LE(pts.front(), 50);
  for (size_t i = 1; i < pts.size(); ++i) {
    EXPECT_GT(pts[i], pts[i - 1]);
    EXPECT_LE(pts[i] - pts[i - 1], 30);
  }
  session.stop();
}

TEST(RecordingSessionTest, HandoffInheritsBindingsAndLateVoiceLayout) {
  RecordingSession previous("unused.mkv", nullptr, nullptr, nullptr, nullptr,
                            false, 128, 3, {});
  previous.setVoiceTrack(2);
  previous.markSourceConnected(2);
  previous.setGameIsFallback(false);
  RecordingSessionTestPeer::bindKnownNode(previous);
  RecordingSession next("unused.mkv", nullptr, nullptr, nullptr, nullptr,
                        false, 128, 2, {});
  next.inheritAudioRouting(previous);
  const auto state = next.audioRoutingState();
  EXPECT_EQ(state.activeTrackCount, 3);
  EXPECT_EQ(state.voiceTrack, 2);
  EXPECT_FALSE(state.gameIsFallback);
  EXPECT_TRUE(state.connected[0]);
  EXPECT_TRUE(state.connected[2]);
  EXPECT_EQ(state.boundSerial[0], "123");
}

TEST(RecordingSessionTest, MissingMergedSlotKeepsHealthyStereoChannelIdentity) {
  if (!haveAac() || !avcodec_find_decoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "AAC encoder/decoder unavailable";
  for (int channel : {0, 1}) {
    SCOPED_TRACE(channel);
    FakeCapture capture;
    FakeVideoEncoder encoder;
    FakeMuxer muxer;
    muxer.collectAudio = true;
    AudioMixer mixer(48000, 2);
    auto source = std::make_unique<FakeAudioSource>();
    auto *raw = source.get();
    mixer.addSource(std::move(source), channel);
    RecordingSession session("unused.mkv", &capture, &encoder, &muxer,
                             &mixer, false, 128, 2, {});
    ASSERT_TRUE(session.start());
    std::vector<uint8_t> video;
    capture.fire(makeSoftwareFrame(video));
    std::vector<float> audio(2048, 0.0f);
    for (int i = 0; i < 1024; ++i)
      audio[2 * i + channel] = 0.2f * std::sin(2 * 3.14159265f * 440 * i / 48000);
    for (int i = 0; i < 20; ++i)
      raw->fire(audio.data(), 1024);
    session.stop();
    std::array<double, 2> energy{0, 0};
    ASSERT_TRUE(decodedChannelEnergy(muxer, energy));
    EXPECT_GT(energy[channel], 1);
    EXPECT_GT(energy[channel], 100 * energy[1 - channel]);
  }
}

TEST(RecordingSessionTest, HandoffSwitchKeepsLiveRouting) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";

  // Simulates ReplayManager::switchSegment: session B starts on the same
  // capture + mixer while A still runs, then A retires with a handoff
  // stop. Frames and samples must keep flowing into B with no restart of
  // the audio source and no detached callbacks.
  FakeCapture capture;
  FakeVideoEncoder encA, encB;
  FakeMuxer muxA, muxB;
  AudioMixer mixer(48000, 2, nullptr);

  auto source = std::make_unique<FakeAudioSource>();
  FakeAudioSource *rawSource = source.get();
  mixer.addSource(std::move(source), 0);

  RecordingSession sessionA(QStringLiteral("/tmp/trophy-test-a.mkv"), &capture,
                            &encA, &muxA, &mixer, true, 128, 3, {});
  ASSERT_TRUE(sessionA.start());
  ASSERT_TRUE(settleMixerSources(mixer));
  EXPECT_EQ(rawSource->starts(), 1);

  std::vector<float> sine(1024 * 2, 0.2f);
  std::vector<uint8_t> storage;
  capture.fire(makeSoftwareFrame(storage));
  for (int i = 0; i < 5; ++i)
    rawSource->fire(sine.data(), 1024);
  EXPECT_GT(muxA.audioPackets(), 0);
  EXPECT_GT(encA.frames(), 0);

  // Successor starts on the live routing: sources must NOT restart.
  RecordingSession sessionB(QStringLiteral("/tmp/trophy-test-b.mkv"), &capture,
                            &encB, &muxB, &mixer, true, 128, 3, {});
  ASSERT_TRUE(sessionB.start());
  EXPECT_EQ(rawSource->starts(), 1);

  // Old leg retires; routing stays with B.
  sessionA.stop(true, true);

  const int aAudio = muxA.audioPackets();
  const int aFrames = encA.frames();
  capture.fire(makeSoftwareFrame(storage));
  for (int i = 0; i < 5; ++i)
    rawSource->fire(sine.data(), 1024);

  // A is finalized: nothing new lands in its file...
  EXPECT_EQ(muxA.audioPackets(), aAudio);
  // ...while B keeps receiving both legs (video encodes async).
  EXPECT_GT(muxB.audioPackets(), 0);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (encB.frames() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_GT(encB.frames(), 0);
  // A's encoder saw no post-handoff frames (callback truly handed over).
  EXPECT_EQ(encA.frames(), aFrames);

  sessionB.stop(false);
}

namespace {

// Observes the production drain's retirement latch, not elapsed time. The
// deadline only prevents a broken implementation from hanging the test suite.
bool waitForRetirement(const AudioMixer::TrackAudioCallbackHandle &handle) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    {
      std::lock_guard<std::mutex> lock(handle->mutex);
      if (!handle->accepting)
        return true;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

class RecordingSessionDrainTest
    : public ::testing::TestWithParam<std::tuple<bool, bool>> {};

TEST_P(RecordingSessionDrainTest, AsyncEnteredAudioDrainKeepsHeartbeatAndSuccessorLive) {
  if (!haveAac()) GTEST_SKIP() << "AAC encoder unavailable";
  const auto [separate, handoff] = GetParam();
  FakeCapture capture;
  FakeVideoEncoder encoder, successorEncoder;
  FakeMuxer muxer, successorMuxer;
  AudioMixer mixer(48000, 2);
  auto source = std::make_unique<FakeAudioSource>();
  auto *raw = source.get();
  bool destroyed = false;
  raw->destroyed = &destroyed;
  mixer.addSource(std::move(source), 0);
  RecordingSession session("unused-a.mkv", &capture, &encoder, &muxer,
                           &mixer, separate, 128, 1);
  RecordingSession successor("unused-b.mkv", &capture, &successorEncoder,
                             &successorMuxer, &mixer, separate, 128, 1);
  ASSERT_TRUE(session.start());
  ASSERT_TRUE(settleMixerSources(mixer));
  std::vector<uint8_t> video;
  capture.fire(makeSoftwareFrame(video));
  std::vector<float> samples(2048, 0.2f);
  muxer.holdNextAudio();
  std::thread audio([&] {
    for (int i = 0; i < 8; ++i) raw->fire(samples.data(), 1024);
  });
  if (!muxer.waitForEnteredAudio()) {
    muxer.releaseAudio(); audio.join();
    FAIL() << "Audio callback did not enter gate";
  }
  if (handoff) {
    EXPECT_TRUE(successor.start());
  }
  QSignalSpy finished(&session, &RecordingSession::stopFinished);
  session.requestStop(handoff, handoff);
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_TRUE(session.stopPending());
  EXPECT_FALSE(session.start()); // No dependency/input reuse while retiring.
  EXPECT_FALSE(destroyed);
  EXPECT_EQ(muxer.finalizations(), 0);
  if (handoff) {
    capture.fire(makeSoftwareFrame(video));
    for (int i = 0; i < 8; ++i) raw->fire(samples.data(), 1024);
    EXPECT_GT(successorMuxer.audioPackets(), 0);
    EXPECT_EQ(raw->starts(), 1);
  }
  muxer.releaseAudio();
  audio.join();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (session.stopPending() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(session.stopPending());
  EXPECT_EQ(finished.count(), 1);
  EXPECT_EQ(muxer.finalizations(), 1);
  EXPECT_TRUE(session.outputSucceeded());
  EXPECT_EQ(destroyed, !handoff);
  if (handoff) successor.stop();
}

TEST(RecordingSessionTest, AsyncVideoJoinKeepsHeartbeatUntilEncoderReturns) {
  FakeCapture capture;
  FakeVideoEncoder encoder;
  FakeMuxer muxer;
  RecordingSession session("unused-video.mkv", &capture, &encoder, &muxer,
                           nullptr, true, 128, 0);
  auto entered = std::make_shared<std::promise<void>>();
  std::promise<void> release;
  auto released = release.get_future().share();
  encoder.encodeGate = [entered, released] {
    entered->set_value();
    released.wait_for(std::chrono::seconds(5));
  };
  ASSERT_TRUE(session.start());
  std::vector<uint8_t> video;
  capture.fire(makeSoftwareFrame(video));
  ASSERT_EQ(entered->get_future().wait_for(std::chrono::seconds(3)), std::future_status::ready);
  session.requestStop();
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_TRUE(session.stopPending());
  EXPECT_EQ(muxer.finalizations(), 0);
  release.set_value();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (session.stopPending() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(session.stopPending());
  EXPECT_EQ(muxer.finalizations(), 1);
}

TEST_P(RecordingSessionDrainTest, EnteredAudioDrainsBeforeFlushAndFinalize) {
  if (!haveAac())
    GTEST_SKIP() << "AAC encoder unavailable";
  const auto [separateTracks, handoff] = GetParam();
  FakeCapture capture;
  FakeVideoEncoder encA, encB;
  FakeMuxer muxA, muxB;
  AudioMixer mixer(48000, 2, nullptr);
  auto source = std::make_unique<FakeAudioSource>();
  auto *rawSource = source.get();
  mixer.addSource(std::move(source), 0);
  RecordingSession sessionA(QStringLiteral("/tmp/trophy-drain-a.mkv"), &capture,
                            &encA, &muxA, &mixer, separateTracks, 128, 1, {});
  RecordingSession sessionB(QStringLiteral("/tmp/trophy-drain-b.mkv"), &capture,
                            &encB, &muxB, &mixer, separateTracks, 128, 1, {});
  ASSERT_TRUE(sessionA.start());
  ASSERT_TRUE(settleMixerSources(mixer));
  const auto oldRegistration = RecordingSessionTestPeer::registration(sessionA);
  ASSERT_NE(oldRegistration, nullptr);
  std::vector<uint8_t> storage;
  capture.fire(makeSoftwareFrame(storage));
  std::vector<float> samples(1024 * 2, 0.2f);

  // AAC buffers its first input. Drive enough frames to enter a real encoded
  // packet write, then hold it with the session's encoder lock still owned.
  muxA.holdNextAudio();
  std::thread audio([&] {
    for (int i = 0; i < 8; ++i)
      rawSource->fire(samples.data(), 1024);
  });
  const bool entered = muxA.waitForEnteredAudio();
  if (!entered) {
    muxA.releaseAudio();
    audio.join();
    FAIL() << "Real session audio callback did not reach the muxer gate";
  }
  const int heldAudioWrites = muxA.audioPackets();

  // Make-before-break: replacing the published registration does not cancel
  // an already-entered old callback, and must not restart the audio source.
  const bool successorStarted = !handoff || sessionB.start();
  EXPECT_TRUE(successorStarted);
  EXPECT_EQ(rawSource->starts(), 1);
  std::promise<void> stopped;
  auto stopDone = stopped.get_future();
  std::thread stopper([&] {
    sessionA.stop(handoff, handoff);
    stopped.set_value();
  });
  const bool retired = waitForRetirement(oldRegistration);
  EXPECT_TRUE(retired);
  if (retired) {
    std::lock_guard<std::mutex> lock(oldRegistration->mutex);
    EXPECT_EQ(oldRegistration->inFlight, 1u);
  }
  // Once the drain has retired the registration, stop may not flush audio
  // into the muxer or finalize it while the entered write remains held.
  EXPECT_EQ(stopDone.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);
  EXPECT_EQ(muxA.audioPackets(), heldAudioWrites);
  EXPECT_EQ(muxA.finalizations(), 0);

  if (handoff && successorStarted) {
    // Exercise successor routing WHILE the old leg is draining, not only
    // after stop returns. These are independent session encoder locks.
    capture.fire(makeSoftwareFrame(storage));
    for (int i = 0; i < 8; ++i)
      rawSource->fire(samples.data(), 1024);
    EXPECT_GT(muxB.audioPackets(), 0);
    EXPECT_EQ(muxB.finalizations(), 0);
    EXPECT_EQ(rawSource->starts(), 1);
    EXPECT_EQ(muxA.audioPackets(), heldAudioWrites);
  }

  // Always release/join before any fatal assertion or object destruction.
  muxA.releaseAudio();
  audio.join();
  stopper.join();
  EXPECT_EQ(stopDone.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
  {
    std::lock_guard<std::mutex> lock(oldRegistration->mutex);
    EXPECT_EQ(oldRegistration->inFlight, 0u);
  }
  EXPECT_EQ(muxA.finalizations(), 1);
  // Buffered AAC must flush only after the entered callback was released.
  EXPECT_GT(muxA.audioPackets(), heldAudioWrites);

  const int settledAudio = muxA.audioPackets();
  rawSource->fire(samples.data(), 1024);
  EXPECT_EQ(muxA.audioPackets(), settledAudio);
  if (handoff && successorStarted) {
    const int bAudio = muxB.audioPackets();
    for (int i = 0; i < 4; ++i)
      rawSource->fire(samples.data(), 1024);
    EXPECT_GT(muxB.audioPackets(), bAudio);
    sessionB.stop();
    // stop joins the encoding thread: no video polling/sleeps needed.
    EXPECT_GT(encB.frames(), 0);
    EXPECT_EQ(muxB.finalizations(), 1);
  }
}

INSTANTIATE_TEST_SUITE_P(SeparateAndMergedStopAndHandoff,
                        RecordingSessionDrainTest,
                        ::testing::Combine(::testing::Bool(),
                                           ::testing::Bool()));

} // namespace
