/*
 * Trophy - Unit Tests
 * AudioMixer: track control, muting, level metering, callback routing.
 */

#include <QCoreApplication>
#include <QSignalSpy>
#include <gtest/gtest.h>

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
