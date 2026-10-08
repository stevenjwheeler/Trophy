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
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <gtest/gtest.h>

#include "audio/AudioMixer.h"
#include "audio/IAudioSource.h"
#include "capture/IVideoCaptureSource.h"
#include "core/RecordingSession.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

using namespace TrophyBackend;

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

class FakeCapture : public IVideoCaptureSource {
public:
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback cb) override { m_cb = std::move(cb); }
  bool startCapture() override { return true; }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {64, 64}; }
  QString name() const override { return QStringLiteral("fake"); }

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
  bool encodeFrame(const VideoFrame &, EncodedPacket &) override {
    ++m_frames;
    return false; // video content irrelevant here; drop it
  }
  int flush(std::vector<EncodedPacket> &) override { return 0; }
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
  int addStream(const StreamInfo &) override { return m_streams++; }
  bool writeHeader() override { return true; }
  bool writePacket(const EncodedPacket &packet) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_packets;
    if (packet.streamIndex != 0)
      ++m_audioPackets;
    return true;
  }
  bool finalize() override { return true; }
  QString outputPath() const override { return m_path; }
  int64_t bytesWritten() const override { return 0; }
  QString formatName() const override { return QStringLiteral("fake"); }

  int audioPackets() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_audioPackets;
  }

private:
  mutable std::mutex m_mutex;
  QString m_path;
  int m_streams = 0;
  int m_packets = 0;
  int m_audioPackets = 0;
};

class FakeAudioSource : public IAudioSource {
public:
  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback cb) override { m_cb = std::move(cb); }
  bool start() override {
    ++m_starts;
    m_capturing = true;
    return true;
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
    if (!m_cb)
      return;
    AudioFrame f{};
    f.samples = samples;
    f.frameCount = frames;
    f.channels = 2;
    f.sampleRate = 48000;
    f.timestamp = 0;
    m_cb(f);
  }

private:
  AudioCallback m_cb;
  bool m_capturing = false;
  int m_starts = 0;
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
