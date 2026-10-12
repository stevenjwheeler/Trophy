/*
 * Trophy - Unit Tests
 * AudioMixer: track control, muting, level metering, callback routing.
 */

#include <QCoreApplication>
#include <QSignalSpy>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <future>
#include <mutex>
#include <thread>

#include "audio/AudioMixer.h"
#include "audio/IAudioSource.h"
#include "audio/PipeWireAudioSource.h"

using namespace TrophyBackend;

class AudioMixerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new AudioMixerTestEnvironment);

// Minimal in-process source: no PipeWire daemon required.
class FakeAudioSource : public IAudioSource {
  Q_OBJECT
public:
  explicit FakeAudioSource(SourceType type, const QString &name,
                           QObject *parent = nullptr)
      : IAudioSource(parent), m_type(type), m_name(name) {}

  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback cb) override { m_cb = std::move(cb); }
  bool start() override {
    m_capturing = true;
    emit captureStarted();
    return true;
  }
  void stop() override {
    m_capturing = false;
    emit captureStopped();
  }
  bool isCapturing() const override { return m_capturing; }
  SourceType sourceType() const override { return m_type; }
  QString displayName() const override { return m_name; }
  QString identifier() const override { return m_name; }
  float peakLevel() const override { return 0.0f; }
  void setVolume(float v) override { m_volume = v; }
  float volume() const override { return m_volume; }

  // Test driver: push a fake buffer through the mixer path.
  void emitAudio(const float *samples, int frames) {
    if (m_cb) {
      AudioFrame f{samples, frames, 2, 48000, 0};
      m_cb(f);
    }
  }

private:
  SourceType m_type;
  QString m_name;
  AudioCallback m_cb;
  bool m_capturing = false;
  float m_volume = 1.0f;
};

class AudioMixerTest : public ::testing::Test {
protected:
  AudioMixer mixer{48000, 2};
};

TEST_F(AudioMixerTest, TrackCountIsThree) {
  EXPECT_EQ(mixer.trackCount(), 3);
  EXPECT_EQ(mixer.trackName(0), "Game Audio");
  EXPECT_EQ(mixer.trackName(1), "Voice Chat");
  EXPECT_EQ(mixer.trackName(2), "Microphone");
}

TEST_F(AudioMixerTest, AddSourceAndStartAll) {
  mixer.addSource(
      std::make_unique<FakeAudioSource>(IAudioSource::SourceType::DesktopAudio,
                                        "desktop"),
      0);
  EXPECT_TRUE(mixer.startAll());
  mixer.stopAll();
}

TEST_F(AudioMixerTest, VolumeClampedAndSignaled) {
  QSignalSpy spy(&mixer, &AudioMixer::trackConfigChanged);
  mixer.setTrackVolume(0, 0.5f);
  EXPECT_FLOAT_EQ(mixer.trackVolume(0), 0.5f);
  mixer.setTrackVolume(0, 5.0f); // clamps to the 0..2 range
  EXPECT_FLOAT_EQ(mixer.trackVolume(0), 2.0f);
  mixer.setTrackVolume(0, -1.0f);
  EXPECT_FLOAT_EQ(mixer.trackVolume(0), 0.0f);
  EXPECT_GE(spy.count(), 3);
}

TEST_F(AudioMixerTest, MuteSuppressesCallbackButUnmuteRestores) {
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());

  int callbackCount = 0;
  mixer.setTrackAudioCallback(
      [&](int track, const float *, int, int, int) {
        if (track == 0)
          ++callbackCount;
      });

  const float samples[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 1);

  mixer.setTrackMuted(0, true);
  EXPECT_TRUE(mixer.isTrackMuted(0));
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 1); // muted: no callback

  mixer.setTrackMuted(0, false);
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 2);

  mixer.stopAll();
}

TEST_F(AudioMixerTest, PeakLevelTracksSignal) {
  AudioMixer mixer2{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer2.addSource(std::unique_ptr<IAudioSource>(raw), 1);
  ASSERT_TRUE(mixer2.startAll());

  EXPECT_FLOAT_EQ(mixer2.getTrackLevel(1), 0.0f);
  const float samples[4] = {0.25f, -0.5f, 0.1f, 0.0f};
  raw->emitAudio(samples, 2);
  EXPECT_GT(mixer2.getTrackLevel(1), 0.0f);
  EXPECT_LE(mixer2.getTrackLevel(1), 1.0f);
  mixer2.stopAll();
}

// Helper: fresh mixer, one buffer at constant amplitude, returned meter level.
static float levelForAmplitude(float amp) {
  AudioMixer m{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  m.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  m.startAll();
  const float samples[4] = {amp, amp, amp, amp};
  raw->emitAudio(samples, 2);
  float lvl = m.getTrackLevel(0);
  m.stopAll();
  return lvl;
}

TEST_F(AudioMixerTest, PerceptualMappingLoudReadsAboveHalfBar) {
  // -20dB (0.1) is typical loud content: must read clearly, well above
  // half-bar (linear would park it at a 10% sliver).
  float lvl = levelForAmplitude(0.1f);
  EXPECT_GT(lvl, 0.5f);
  EXPECT_GT(lvl, 0.25f);
  EXPECT_LT(lvl, 1.0f);
  // -12dB (~0.2512) is hot: must read near-full.
  EXPECT_GT(levelForAmplitude(0.25118843f), 0.7f);
}

TEST_F(AudioMixerTest, PerceptualMappingQuietStaysLow) {
  // -40dB (0.01): visible but well below half-bar, never pinned.
  float quiet = levelForAmplitude(0.01f);
  EXPECT_GT(quiet, 0.0f);
  EXPECT_LT(quiet, 0.5f);
  // Silence parks at zero.
  EXPECT_FLOAT_EQ(levelForAmplitude(0.0f), 0.0f);
  // Full scale stays in range.
  float full = levelForAmplitude(1.0f);
  EXPECT_GT(full, 0.95f);
  EXPECT_LE(full, 1.0f);
}

TEST_F(AudioMixerTest, PerceptualMappingClampsAboveFullScale) {
  // Volume gain can push the raw peak past 1.0 — the meter must clamp.
  AudioMixer mixer2{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer2.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  mixer2.setTrackVolume(0, 2.0f);
  ASSERT_TRUE(mixer2.startAll());
  const float samples[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  raw->emitAudio(samples, 2);
  EXPECT_LE(mixer2.getTrackLevel(0), 1.0f);
  mixer2.stopAll();
}

TEST_F(AudioMixerTest, MutedLevelReadsZeroImmediately) {
  AudioMixer mixer2{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer2.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer2.startAll());
  const float samples[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  raw->emitAudio(samples, 2);
  EXPECT_GT(mixer2.getTrackLevel(0), 0.5f);
  mixer2.setTrackMuted(0, true);
  raw->emitAudio(samples, 2);
  EXPECT_FLOAT_EQ(mixer2.getTrackLevel(0), 0.0f);
  mixer2.stopAll();
}

TEST_F(AudioMixerTest, RemoveSourceStopsCallbacks) {
  AudioMixer mixer2{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer2.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer2.startAll());

  int count = 0;
  mixer2.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++count; });

  mixer2.removeSource(0);
  // No source left to drive; callback count must stay put.
  EXPECT_EQ(count, 0);
  mixer2.stopAll();
}

TEST_F(AudioMixerTest, DrainRetiredCallbackWithoutTouchingSuccessor) {
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());

  std::mutex gateMutex;
  std::condition_variable gateCv;
  bool entered = false;
  bool release = false;
  const auto oldHandle = mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) {
        std::unique_lock<std::mutex> lock(gateMutex);
        entered = true;
        gateCv.notify_all();
        gateCv.wait(lock, [&] { return release; });
      });
  std::atomic<int> successorCalls{0};
  std::thread producer([&] {
    const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    raw->emitAudio(samples, 2);
  });
  bool callbackEntered = false;
  {
    std::unique_lock<std::mutex> lock(gateMutex);
    callbackEntered = gateCv.wait_for(lock, std::chrono::seconds(2),
                                       [&] { return entered; });
  }
  if (!callbackEntered) {
    EXPECT_TRUE(callbackEntered);
    producer.join();
    return;
  }

  // Publishing the successor must not prevent draining an already-running
  // callback belonging to the retiring session.
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) {
    ++successorCalls;
  });
  std::promise<void> drainedPromise;
  auto drained = drainedPromise.get_future();
  std::thread drainer([&] {
    mixer.drainTrackAudioCallback(oldHandle);
    drainedPromise.set_value();
  });
  EXPECT_EQ(drained.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(gateMutex);
    release = true;
  }
  gateCv.notify_all();
  producer.join();
  drainer.join();
  EXPECT_EQ(drained.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);

  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  raw->emitAudio(samples, 2);
  EXPECT_EQ(successorCalls.load(), 1);
  mixer.stopAll();
}

// Pinning must prefer serial, then name, and only use the numeric ID as a
// last resort: WirePlumber reads target.object as a *serial*, and IDs
// collide with serials (live: easyeffects_source id 73 / serial 74 vs
// easyeffects_sink serial 73 — pinning bare "73" captured the sink).
TEST(AudioPinTargetTest, PrefersSerialThenNameOverNumericId) {
  using P = PipeWireAudioSource;
  EXPECT_EQ(P::preferredTarget("74", "easyeffects_source", 73), "74");
  EXPECT_EQ(P::preferredTarget("", "easyeffects_source", 73),
            "easyeffects_source");
  EXPECT_EQ(P::preferredTarget("", "", 73), "73");
  // Name wins over a *different* numeric ID: the name resolves
  // unambiguously while the ID may alias another node's serial.
  EXPECT_EQ(P::preferredTarget("", "easyeffects_source", 999),
            "easyeffects_source");
}

TEST_F(AudioMixerTest, HotSwapReplacesRunningSource) {
  // Live retarget (what applyMicrophoneDevice does): replace a track's
  // source mid-run — the late-added one initializes and starts
  // immediately, and it (not the old one) drives the track callback.
  AudioMixer mixer{48000, 2};
  auto *rawA = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                   "a");
  mixer.addSource(std::unique_ptr<IAudioSource>(rawA), 0);
  ASSERT_TRUE(mixer.startAll());
  EXPECT_TRUE(rawA->isCapturing());

  auto *rawB = new FakeAudioSource(IAudioSource::SourceType::Microphone,
                                   "b");
  // NOTE: replacing destroys rawA — do not touch it after this line.
  mixer.addSource(std::unique_ptr<IAudioSource>(rawB), 0);
  EXPECT_TRUE(rawB->isCapturing());

  int count = 0;
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++count; });
  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  rawB->emitAudio(samples, 2);
  EXPECT_EQ(count, 1);
  mixer.stopAll();
  EXPECT_FALSE(rawB->isCapturing());
}

#include "test_AudioMixer.moc"
