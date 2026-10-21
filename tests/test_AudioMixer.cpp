/*
 * Trophy - Unit Tests
 * AudioMixer: track control, muting, level metering, callback routing.
 */

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <future>
#include <mutex>
#include <cmath>
#include <limits>
#include <thread>
#include <tuple>

#include "audio/AudioMixer.h"
#include "audio/IAudioSource.h"
#include "audio/PipeWireAudioSource.h"

using namespace TrophyBackend;

namespace TrophyBackend {
class AudioMixerTestPeer {
public:
  static auto sourceCallback(AudioMixer &mixer, int index) {
    std::lock_guard<std::mutex> lock(mixer.m_mutex);
    return mixer.m_tracks[index].sourceCallbackActive;
  }
};
} // namespace TrophyBackend

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
  void setCallback(AudioCallback cb) override {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    m_cb = std::move(cb);
  }
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

  AudioCallback snapshotCallback() {
    std::lock_guard<std::mutex> lock(m_callbackMutex);
    return m_cb;
  }

  // Test driver: push a fake buffer through the mixer path.
  void emitAudio(const float *samples, int frames) {
    const auto callback = snapshotCallback();
    // Invoke outside the publish lock, like the real source's retained
    // callback snapshot. Tests must not serialize stop against delivery.
    if (callback) {
      AudioFrame f{samples, frames, 2, 48000, 0};
      callback(f);
    }
  }

private:
  SourceType m_type;
  QString m_name;
  AudioCallback m_cb;
  std::mutex m_callbackMutex;
  std::atomic<bool> m_capturing{false};
  std::atomic<float> m_volume{1.0f};
};

TEST(AudioHealthTest, SemanticStatusUsesActualSourcesAndNotSignalLevel) {
  AudioMixer mixer(48000, 2);
  EXPECT_EQ(mixer.semanticSourceStatus(2), "unavailable");
  // Merged routing puts mic in physical slot 1, not logical role 1.
  auto mic = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::Microphone, "mic");
  mixer.addSource(std::move(mic), 1);
  EXPECT_EQ(mixer.semanticSourceStatus(1), "unavailable");
  ASSERT_TRUE(mixer.startAll());
  EXPECT_EQ(mixer.semanticSourceStatus(2), "capturing");
  EXPECT_FLOAT_EQ(mixer.semanticTrackLevel(2), 0.0f);
  mixer.stopAll();
  EXPECT_EQ(mixer.semanticSourceStatus(2), "unavailable");
}

TEST(AudioHealthTest, SemanticNameReportsBoundSourceName) {
  AudioMixer mixer(48000, 2);
  EXPECT_TRUE(mixer.semanticSourceName(0).isEmpty());
  auto desktop = std::make_unique<FakeAudioSource>(
      IAudioSource::SourceType::DesktopAudio, "Desktop Audio");
  mixer.addSource(std::move(desktop), 0);
  // Merged routing binds mic to physical slot 1 but logical role 2.
  auto mic = std::make_unique<FakeAudioSource>(
      IAudioSource::SourceType::Microphone, "USB Microphone");
  mixer.addSource(std::move(mic), 1);
  EXPECT_EQ(mixer.semanticSourceName(0), "Desktop Audio");
  EXPECT_EQ(mixer.semanticSourceName(2), "USB Microphone");
  EXPECT_TRUE(mixer.semanticSourceName(1).isEmpty());
}

TEST(AudioSignalStatsTest, WindowsWeightSamplePowerAndExpireWithoutNewBuffers) {
  AudioSignalStats stats;
  const int64_t now = 10000000;
  EXPECT_FALSE(stats.summary(now).received);
  stats.record(now, 1.0, 1.0, 2, 1);
  stats.record(now, 0.5, 0.5, 2, 0);
  auto recent = stats.summary(now);
  EXPECT_TRUE(recent.received);
  EXPECT_TRUE(recent.recent);
  EXPECT_DOUBLE_EQ(recent.peak, 1.0);
  EXPECT_DOUBLE_EQ(recent.rms, std::sqrt(1.5 / 4));
  EXPECT_TRUE(recent.clipping);
  EXPECT_EQ(recent.clippedSamples, 1);
  auto stalled = stats.summary(now + 1000000);
  EXPECT_FALSE(stalled.recent);
  EXPECT_TRUE(stalled.held);
  EXPECT_EQ(stalled.ageMs, 1000);
  EXPECT_DOUBLE_EQ(stalled.heldPeak, 1.0);
  auto expired = stats.summary(now + 5000000);
  EXPECT_FALSE(expired.held);
  EXPECT_FALSE(expired.clipping);
  EXPECT_EQ(expired.totalClippedSamples, 1);
  // High callback cadence does not shorten either time window.
  for (int i = 0; i < 10000; ++i) stats.record(now + 7000000, 0.1, 0.02, 2, 0);
  auto silentThenQuiet = stats.summary(now + 7000000);
  EXPECT_DOUBLE_EQ(silentThenQuiet.peak, 0.1);
  EXPECT_NEAR(silentThenQuiet.rms, 0.1, 1e-10);
  EXPECT_EQ(silentThenQuiet.totalClippedSamples, 1);
}

TEST(AudioSignalStatsTest, ConcurrentPublicationKeepsSnapshotsFinite) {
  AudioSignalStats stats;
  std::atomic<bool> done{false};
  std::thread writer([&] {
    for (int i = 0; i < 10000; ++i) stats.record(10000000, 0.5, 0.5, 2, 0);
    done = true;
  });
  do {
    const auto summary = stats.summary(10000000);
    if (summary.recent) {
      EXPECT_DOUBLE_EQ(summary.peak, 0.5);
      EXPECT_DOUBLE_EQ(summary.rms, 0.5);
    }
  } while (!done);
  writer.join();
}

TEST(AudioHealthTest, DiagnosticsUsePostMutePcmAndResetOnSourceReplacement) {
  AudioMixer mixer(48000, 2);
  auto mic = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::Microphone, "mic");
  auto *rawMic = mic.get();
  mixer.addSource(std::move(mic), 1); // Mic is logical role 2, not Voice.
  ASSERT_TRUE(mixer.startAll());
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(2)["received"].toBool());
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(2).contains("peak"));
  const float samples[] = {0.5f, -0.25f, 0.0f, 1.25f};
  rawMic->emitAudio(samples, 2);
  const auto signal = mixer.semanticAudioDiagnostics(2);
  EXPECT_TRUE(signal["received"].toBool());
  EXPECT_DOUBLE_EQ(signal["peak"].toDouble(), 1.25);
  EXPECT_DOUBLE_EQ(signal["rms"].toDouble(), std::sqrt(1.875 / 4));
  EXPECT_EQ(signal["fullScaleSamplesTotal"].toULongLong(), 1);
  EXPECT_TRUE(signal["clippingRisk"].toBool());
  EXPECT_TRUE(signal.contains("lastBufferAgeMs"));
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(1)["received"].toBool());
  mixer.setSemanticTrackMuted(2, true);
  EXPECT_DOUBLE_EQ(mixer.semanticAudioDiagnostics(2)["peak"].toDouble(), 0);
  std::vector<float> delivered;
  mixer.setTrackAudioCallback([&](int, const float *pcm, int frames, int, int channels) {
    delivered.assign(pcm, pcm + frames * channels);
  });
  rawMic->emitAudio(samples, 2);
  ASSERT_EQ(delivered.size(), 4);
  for (float sample : delivered) EXPECT_FLOAT_EQ(sample, 0);
  EXPECT_FLOAT_EQ(samples[3], 1.25f); // borrowed PCM was not modified
  const auto muted = mixer.semanticAudioDiagnostics(2);
  EXPECT_TRUE(muted["received"].toBool());
  EXPECT_TRUE(muted["muted"].toBool());
  EXPECT_DOUBLE_EQ(muted["rms"].toDouble(), 0);
  EXPECT_DOUBLE_EQ(muted["peakHold"].toDouble(), 1.25); // historical, not current output
  EXPECT_EQ(muted["fullScaleSamplesTotal"].toULongLong(), 1);
  const auto retired = rawMic->snapshotCallback();
  auto replacement = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::Microphone, "new mic");
  auto *rawReplacement = replacement.get();
  mixer.addSource(std::move(replacement), 1);
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(2)["received"].toBool());
  retired(AudioFrame{samples, 2, 2, 48000, 0});
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(2)["received"].toBool());
  const float silence[] = {0, 0};
  rawReplacement->emitAudio(silence, 1);
  EXPECT_TRUE(mixer.semanticAudioDiagnostics(2)["received"].toBool());
  EXPECT_DOUBLE_EQ(mixer.semanticAudioDiagnostics(2)["peak"].toDouble(), 0);
  EXPECT_EQ(mixer.semanticAudioDiagnostics(2)["fullScaleSamplesTotal"].toULongLong(), 0);
  mixer.removeSource(1);
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(2)["received"].toBool());
}

TEST(AudioHealthTest, NonFiniteSamplesDoNotPoisonSignalMeasurements) {
  AudioMixer mixer(48000, 2);
  auto source = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::ApplicationAudio, "game");
  auto *raw = source.get();
  mixer.addSource(std::move(source), 0);
  ASSERT_TRUE(mixer.startAll());
  raw->emitAudio(nullptr, 0);
  EXPECT_FALSE(mixer.semanticAudioDiagnostics(0)["received"].toBool());
  const float pcm[] = {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity(), 0.5f};
  raw->emitAudio(pcm, 2);
  auto signal = mixer.semanticAudioDiagnostics(0);
  EXPECT_DOUBLE_EQ(signal["peak"].toDouble(), 0.5);
  EXPECT_DOUBLE_EQ(signal["rms"].toDouble(), 0.25);
  EXPECT_FALSE(signal["clippingRisk"].toBool());
}

// All gates self-release on timeout so even a failed assertion can join
// its worker. State is separate from the source so destruction is observable
// without ever dereferencing a retired raw pointer.
struct AudioSourceGate {
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool released = false;
  std::atomic<bool> timedOut{false};
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  std::atomic<int> destructions{0};
  std::atomic<bool> capturing{false};
  QThread *owner = QThread::currentThread();
  std::atomic<bool> destroyedOnOwner{false};

  void block() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    cv.notify_all();
    timedOut = !cv.wait_for(lock, std::chrono::seconds(5),
                            [&] { return released; });
  }
  bool waitForEntry() {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2),
                       [&] { return entered; });
  }
  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    cv.notify_all();
  }
};

class GatedAudioSource : public FakeAudioSource {
public:
  GatedAudioSource(std::shared_ptr<AudioSourceGate> gate, bool blockInitialize,
                   bool initializeResult = true,
                   SourceType type = SourceType::DesktopAudio)
      : FakeAudioSource(type, "gated"),
        m_gate(std::move(gate)), m_blockInitialize(blockInitialize),
        m_initializeResult(initializeResult) {}
  ~GatedAudioSource() override {
    m_gate->destroyedOnOwner = QThread::currentThread() == m_gate->owner;
    ++m_gate->destructions;
  }
  bool initialize(const AudioConfig &) override {
    if (m_blockInitialize)
      m_gate->block();
    return m_initializeResult;
  }
  bool start() override {
    ++m_gate->starts;
    m_gate->capturing = true;
    return FakeAudioSource::start();
  }
  void stop() override {
    ++m_gate->stops;
    if (!m_blockInitialize)
      m_gate->block();
    m_gate->capturing = false;
    FakeAudioSource::stop();
  }

private:
  std::shared_ptr<AudioSourceGate> m_gate;
  bool m_blockInitialize;
  bool m_initializeResult;
};

class FirstBufferGatedSource : public GatedAudioSource {
public:
  FirstBufferGatedSource(std::shared_ptr<AudioSourceGate> gate, SourceType type)
      : GatedAudioSource(std::move(gate), true, true, type) {}
  bool start() override {
    const bool result = GatedAudioSource::start();
    const float samples[2] = {0.6f, 0.6f};
    emitAudio(samples, 1);
    return result;
  }
};

class FailOnceFirstBufferSource : public FakeAudioSource {
public:
  FailOnceFirstBufferSource() : FakeAudioSource(SourceType::ApplicationAudio, "voice") {}
  bool initialize(const AudioConfig &) override { return ++attempts > 1; }
  bool start() override {
    const bool result = FakeAudioSource::start();
    const float samples[2] = {0.6f, 0.6f};
    emitAudio(samples, 1);
    return result;
  }
  std::atomic<int> attempts{0};
};

class CallbackClearSource : public FakeAudioSource {
public:
  explicit CallbackClearSource(std::shared_ptr<bool> cleared)
      : FakeAudioSource(IAudioSource::SourceType::DesktopAudio, "desktop"),
        m_cleared(std::move(cleared)) {}
  void setCallback(AudioCallback cb) override {
    if (!cb)
      *m_cleared = true;
    FakeAudioSource::setCallback(std::move(cb));
  }

private:
  std::shared_ptr<bool> m_cleared;
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

static bool waitForAsyncTrack(AudioMixer &mixer, int track) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (mixer.sourceOperationPending(track) &&
         std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return !mixer.sourceOperationPending(track);
}

TEST_F(AudioMixerTest, AsyncInitializeKeepsOwnerResponsiveAndDropsOldStart) {
  auto old = std::make_shared<AudioSourceGate>();
  mixer.addSourceAsync(std::make_unique<GatedAudioSource>(old, true), 0);
  mixer.ensureSourcesStartedAsync();
  ASSERT_TRUE(old->waitForEntry());
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  auto intermediate = std::make_shared<AudioSourceGate>();
  intermediate->release();
  auto latest = std::make_shared<AudioSourceGate>();
  latest->release();
  mixer.addSourceAsync(std::make_unique<GatedAudioSource>(intermediate, true), 0);
  mixer.addSourceAsync(std::make_unique<GatedAudioSource>(latest, true), 0);
  EXPECT_EQ(old->destructions, 0);
  EXPECT_EQ(latest->starts, 0); // One worker for this track, not overlapping.
  old->release();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 0));
  EXPECT_FALSE(old->capturing);
  EXPECT_EQ(intermediate->starts, 0);
  EXPECT_EQ(latest->starts, 1);
  EXPECT_TRUE(mixer.sourceIsCapturing(0));
  EXPECT_EQ(old->destructions, 1);
  EXPECT_TRUE(old->destroyedOnOwner);
  EXPECT_TRUE(intermediate->destroyedOnOwner);
}

TEST_F(AudioMixerTest, AsyncRetirementKeepsOwnerResponsiveAndRetainsSource) {
  auto old = std::make_shared<AudioSourceGate>();
  mixer.addSource(std::make_unique<GatedAudioSource>(old, false), 0);
  ASSERT_TRUE(mixer.startAll());
  mixer.removeSourceAsync(0);
  ASSERT_TRUE(old->waitForEntry());
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_EQ(old->destructions, 0);
  auto replacement = std::make_shared<AudioSourceGate>();
  replacement->release();
  mixer.addSourceAsync(std::make_unique<GatedAudioSource>(replacement, true), 0);
  EXPECT_EQ(replacement->starts, 0);
  old->release();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 0));
  EXPECT_EQ(old->destructions, 1);
  EXPECT_TRUE(old->destroyedOnOwner);
  EXPECT_EQ(replacement->starts, 1);
}

TEST_F(AudioMixerTest, AsyncSourceDrainKeepsOwnerResponsiveUntilCallbackReturns) {
  auto native = std::make_shared<AudioSourceGate>();
  native->release();
  auto source = std::make_unique<GatedAudioSource>(native, true);
  auto *raw = source.get();
  mixer.addSource(std::move(source), 0);
  ASSERT_TRUE(mixer.startAll());
  auto callback = std::make_shared<AudioSourceGate>();
  mixer.setTrackAudioCallback([callback](int, const float *, int, int, int) {
    callback->block();
  });
  auto delivery = std::async(std::launch::async, [raw] {
    const float samples[2] = {0.1f, 0.1f};
    raw->emitAudio(samples, 1);
  });
  ASSERT_TRUE(callback->waitForEntry());
  mixer.removeSourceAsync(0);
  bool heartbeat = false;
  QTimer::singleShot(0, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_TRUE(mixer.sourceOperationPending(0));
  EXPECT_EQ(native->destructions, 0);
  callback->release();
  delivery.get();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 0));
  EXPECT_EQ(native->destructions, 1);
  EXPECT_TRUE(native->destroyedOnOwner);
}

TEST_F(AudioMixerTest, SemanticRolesKeepMergedMicAndLateVoiceIndependent) {
  auto game = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::ApplicationAudio, "game");
  auto mic = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::Microphone, "mic");
  auto *rawGame = game.get();
  auto *rawMic = mic.get();
  mixer.addSource(std::move(game), 0);
  mixer.addSource(std::move(mic), 1);
  ASSERT_TRUE(mixer.startAll());
  std::array<float, 3> delivered{};
  mixer.setTrackAudioCallback([&](int track, const float *samples, int, int, int) {
    delivered[track] = samples[0];
  });
  const float samples[2] = {0.4f, 0.4f};
  rawGame->emitAudio(samples, 1);
  rawMic->emitAudio(samples, 1);
  EXPECT_GT(mixer.semanticTrackLevel(0), 0.0f);
  EXPECT_GT(mixer.semanticTrackLevel(2), 0.0f);
  EXPECT_FLOAT_EQ(mixer.semanticTrackLevel(1), 0.0f);
  mixer.setSemanticTrackMuted(1, true); // Absent voice must not mute physical mic=1.
  EXPECT_FALSE(mixer.isTrackMuted(1));
  rawMic->emitAudio(samples, 1);
  EXPECT_FLOAT_EQ(delivered[1], 0.4f);
  auto voice = std::make_unique<FakeAudioSource>(IAudioSource::SourceType::ApplicationAudio, "voice");
  auto *rawVoice = voice.get();
  mixer.addSource(std::move(voice), 2);
  EXPECT_TRUE(mixer.isTrackMuted(2));
  rawVoice->emitAudio(samples, 1);
  EXPECT_FLOAT_EQ(delivered[2], 0.0f);
  EXPECT_NEAR(mixer.semanticAudioDiagnostics(2)["peak"].toDouble(), 0.4, 1e-6);
  EXPECT_DOUBLE_EQ(mixer.semanticAudioDiagnostics(1)["peak"].toDouble(), 0.0);
  EXPECT_GT(mixer.semanticTrackLevel(2), 0.0f);
  mixer.setSemanticTrackMuted(2, true);
  mixer.setSemanticTrackMuted(1, false);
  EXPECT_TRUE(mixer.isTrackMuted(1));
  EXPECT_FALSE(mixer.isTrackMuted(2));
  rawMic->emitAudio(samples, 1);
  rawVoice->emitAudio(samples, 1);
  EXPECT_FLOAT_EQ(delivered[1], 0.0f);
  EXPECT_FLOAT_EQ(delivered[2], 0.4f);
  EXPECT_FLOAT_EQ(mixer.semanticTrackLevel(2), 0.0f);
  EXPECT_GT(mixer.semanticTrackLevel(1), 0.0f);
  mixer.setTrackMuted(1, false); // Legacy physical APIs still target their slot.
  EXPECT_FALSE(mixer.isSemanticTrackMuted(2));
}

TEST(AudioSemanticRoleTest, IdleAndPendingPreferencesApplyToFirstNativeBuffer) {
  for (bool finalMute : {true, false}) {
    SCOPED_TRACE(finalMute);
    AudioMixer mixer(48000, 2);
    mixer.setSemanticTrackMuted(2, true);
    mixer.setSemanticTrackMuted(1, true);
    auto gate = std::make_shared<AudioSourceGate>();
    auto deliveries = std::make_shared<std::atomic<int>>(0);
    auto firstSample = std::make_shared<std::atomic<float>>(-1.0f);
    mixer.setTrackAudioCallback([deliveries, firstSample](int track, const float *samples, int, int, int) {
      EXPECT_EQ(track, 1);
      *firstSample = samples[0];
      ++*deliveries;
    });
    mixer.addSourceAsync(std::make_unique<FirstBufferGatedSource>(
        gate, IAudioSource::SourceType::Microphone), 1);
    mixer.ensureSourcesStartedAsync();
    ASSERT_TRUE(gate->waitForEntry());
    EXPECT_TRUE(mixer.isTrackMuted(1));
    mixer.setSemanticTrackMuted(2, finalMute);
    EXPECT_EQ(mixer.isTrackMuted(1), finalMute);
    EXPECT_TRUE(mixer.isSemanticTrackMuted(1)); // Independent absent voice preference.
    gate->release();
    ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
    EXPECT_EQ(deliveries->load(), 1);
    EXPECT_FLOAT_EQ(firstSample->load(), finalMute ? 0.0f : 0.6f);
  }
}

TEST_F(AudioMixerTest, ChangingSlotRoleClearsOldMuteBeforeNativeStartup) {
  mixer.setSemanticTrackMuted(1, true);
  mixer.setSemanticTrackMuted(2, false);
  mixer.addSource(std::make_unique<FakeAudioSource>(
      IAudioSource::SourceType::ApplicationAudio, "voice"), 1);
  ASSERT_TRUE(mixer.startAll());
  EXPECT_TRUE(mixer.isTrackMuted(1));
  auto firstSample = std::make_shared<std::atomic<float>>(-1.0f);
  mixer.setTrackAudioCallback([firstSample](int, const float *samples, int, int, int) {
    *firstSample = samples[0];
  });
  auto micGate = std::make_shared<AudioSourceGate>();
  mixer.addSourceAsync(std::make_unique<FirstBufferGatedSource>(
      micGate, IAudioSource::SourceType::Microphone), 1);
  ASSERT_TRUE(micGate->waitForEntry());
  EXPECT_FALSE(mixer.isTrackMuted(1));
  EXPECT_TRUE(mixer.isSemanticTrackMuted(1));
  micGate->release();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  EXPECT_FLOAT_EQ(firstSample->load(), 0.6f);
  EXPECT_GT(mixer.semanticTrackLevel(2), 0.0f);
  auto voiceGate = std::make_shared<AudioSourceGate>();
  mixer.addSourceAsync(std::make_unique<FirstBufferGatedSource>(
      voiceGate, IAudioSource::SourceType::ApplicationAudio), 1);
  ASSERT_TRUE(voiceGate->waitForEntry());
  EXPECT_TRUE(mixer.isTrackMuted(1));
  voiceGate->release();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  EXPECT_FLOAT_EQ(firstSample->load(), 0.0f);
  EXPECT_FLOAT_EQ(mixer.semanticTrackLevel(2), 0.0f);
}

TEST_F(AudioMixerTest, SemanticPreferencesSurviveFailureRetryAndSlotRelayout) {
  mixer.setSemanticTrackMuted(1, true);
  mixer.setSemanticTrackMuted(2, false);
  auto delivered = std::make_shared<std::array<std::atomic<float>, 3>>();
  mixer.setTrackAudioCallback([delivered](int track, const float *samples, int, int, int) {
    (*delivered)[track] = samples[0];
  });
  auto voice = std::make_unique<FailOnceFirstBufferSource>();
  auto *raw = voice.get();
  mixer.addSourceAsync(std::move(voice), 1);
  mixer.ensureSourcesStartedAsync();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  EXPECT_FALSE(mixer.sourceIsCapturing(1));
  EXPECT_TRUE(mixer.isSemanticTrackMuted(1));
  mixer.ensureSourcesStartedAsync();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  EXPECT_EQ(raw->attempts, 2);
  EXPECT_TRUE(mixer.sourceIsCapturing(1));
  EXPECT_FLOAT_EQ((*delivered)[1], 0.0f);
  mixer.stopSourcesAsync();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  EXPECT_TRUE(mixer.isSemanticTrackMuted(1));
  auto micGate = std::make_shared<AudioSourceGate>(); micGate->release();
  auto voiceGate = std::make_shared<AudioSourceGate>(); voiceGate->release();
  mixer.addSourceAsync(std::make_unique<FirstBufferGatedSource>(
      micGate, IAudioSource::SourceType::Microphone), 1);
  mixer.addSourceAsync(std::make_unique<FirstBufferGatedSource>(
      voiceGate, IAudioSource::SourceType::ApplicationAudio), 2);
  EXPECT_FALSE(mixer.isTrackMuted(1));
  EXPECT_TRUE(mixer.isTrackMuted(2));
  mixer.ensureSourcesStartedAsync();
  ASSERT_TRUE(waitForAsyncTrack(mixer, 1));
  ASSERT_TRUE(waitForAsyncTrack(mixer, 2));
  EXPECT_FLOAT_EQ((*delivered)[1], 0.6f);
  EXPECT_FLOAT_EQ((*delivered)[2], 0.0f);
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

TEST_F(AudioMixerTest, MutePreservesCallbackCadenceWithSilence) {
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());

  int callbackCount = 0;
  std::vector<float> received;
  mixer.setTrackAudioCallback(
      [&](int track, const float *data, int frames, int, int channels) {
        if (track == 0) {
          ++callbackCount;
          received.assign(data, data + frames * channels);
        }
      });

  const float samples[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 1);

  mixer.setTrackMuted(0, true);
  EXPECT_TRUE(mixer.isTrackMuted(0));
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 2);
  EXPECT_EQ(received, std::vector<float>(4, 0.0f));
  EXPECT_FLOAT_EQ(samples[0], 0.5f); // borrowed input was not silenced

  mixer.setTrackMuted(0, false);
  raw->emitAudio(samples, 2);
  EXPECT_EQ(callbackCount, 3);
  EXPECT_EQ(received, std::vector<float>(4, 0.5f));

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

TEST_F(AudioMixerTest, RemoveSourceUnpublishesCallbackBeforeStop) {
  // A detached source may still dispatch a frame while stop() tears down
  // PipeWire. Its callback must be cleared before stop, or that frame
  // could reach a new source occupying the same track index.
  auto cleared = std::make_shared<bool>(false);
  mixer.addSource(std::make_unique<CallbackClearSource>(cleared), 0);
  ASSERT_TRUE(mixer.startAll());
  mixer.removeSource(0);
  EXPECT_TRUE(*cleared);
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
        EXPECT_TRUE(gateCv.wait_for(lock, std::chrono::seconds(5),
                                    [&] { return release; }));
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
    {
      std::lock_guard<std::mutex> lock(gateMutex);
      release = true;
    }
    gateCv.notify_all();
    producer.join();
    return;
  }

  // Publishing the successor must not prevent draining an already-running
  // callback belonging to the retiring session.
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) {
    ++successorCalls;
  });
  const float successorSamples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  raw->emitAudio(successorSamples, 2);
  EXPECT_EQ(successorCalls.load(), 1);
  std::promise<void> drainedPromise;
  auto drained = drainedPromise.get_future();
  std::thread drainer([&] {
    mixer.drainTrackAudioCallback(oldHandle);
    drainedPromise.set_value();
  });
  // Observe retirement itself, rather than treating an unscheduled
  // drainer as evidence that it waited for the producer.
  bool retired = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(1);
  while (!retired && std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(oldHandle->mutex);
      retired = !oldHandle->accepting;
    }
    if (!retired)
      std::this_thread::yield();
  }
  EXPECT_TRUE(retired);
  EXPECT_EQ(drained.wait_for(std::chrono::milliseconds(0)),
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
  EXPECT_EQ(successorCalls.load(), 2);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, RemoveDuringInitializeRetainsSourceAndRejectsLateStart) {
  auto gate = std::make_shared<AudioSourceGate>();
  mixer.addSource(std::make_unique<GatedAudioSource>(gate, true), 0);
  bool started = true;
  std::thread starter([&] { started = mixer.startAll(); });
  const bool entered = gate->waitForEntry();
  EXPECT_TRUE(entered);

  std::promise<void> replacedPromise;
  auto replaced = replacedPromise.get_future();
  auto *successor = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                       "successor");
  std::thread replacer([&] {
    mixer.removeSource(0);
    mixer.addSource(std::unique_ptr<IAudioSource>(successor), 0);
    replacedPromise.set_value();
  });
  const bool replacedWhileBlocked =
      replaced.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  EXPECT_TRUE(replacedWhileBlocked);
  if (entered && replacedWhileBlocked) {
    EXPECT_EQ(gate->destructions.load(), 0);
    EXPECT_EQ(gate->starts.load(), 0);
    EXPECT_TRUE(successor->isCapturing());
  }
  gate->release();
  starter.join();
  replacer.join();

  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_FALSE(started); // The removed startup cannot report success.
  EXPECT_EQ(gate->starts.load(), 1);
  EXPECT_GE(gate->stops.load(), 2); // Removal and stale-start convergence.
  EXPECT_FALSE(gate->capturing.load());
  EXPECT_EQ(gate->destructions.load(), 1);
  EXPECT_TRUE(successor->isCapturing());
  int calls = 0;
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) { ++calls; });
  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  successor->emitAudio(samples, 2);
  EXPECT_EQ(calls, 1);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, RemovedSourceCannotDeliverWhileStopIsBlocked) {
  auto gate = std::make_shared<AudioSourceGate>();
  auto *retired = new GatedAudioSource(gate, false);
  mixer.addSource(std::unique_ptr<IAudioSource>(retired), 0);
  ASSERT_TRUE(mixer.startAll());
  std::atomic<int> calls{0};
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++calls; });
  std::thread remover([&] { mixer.removeSource(0); });
  const bool entered = gate->waitForEntry();
  EXPECT_TRUE(entered);
  if (entered) {
    std::lock_guard<std::mutex> lock(gate->mutex);
    // stop() retains the source until release; the callback must already
    // be unpublished, even though the detached source still exists.
    // Holding the gate lock also prevents its timeout from destroying
    // the source while the main thread uses this borrowed pointer.
    if (!gate->timedOut.load()) {
      EXPECT_EQ(gate->destructions.load(), 0);
      const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
      retired->emitAudio(samples, 2);
      EXPECT_EQ(calls.load(), 0);
    }
  }
  gate->release();
  remover.join();
  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_EQ(gate->destructions.load(), 1);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, StopDuringInitializeRejectsLateStartButAllowsRestart) {
  auto gate = std::make_shared<AudioSourceGate>();
  mixer.addSource(std::make_unique<GatedAudioSource>(gate, true), 0);
  bool started = true;
  std::thread starter([&] { started = mixer.startAll(); });
  EXPECT_TRUE(gate->waitForEntry());
  std::promise<void> stoppedPromise;
  auto stopped = stoppedPromise.get_future();
  std::thread stopper([&] {
    mixer.stopAll();
    stoppedPromise.set_value();
  });
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  gate->release();
  starter.join();
  stopper.join();
  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_FALSE(started);
  EXPECT_FALSE(gate->capturing.load());
  EXPECT_EQ(gate->starts.load(), 1);
  EXPECT_GE(gate->stops.load(), 2);
  EXPECT_EQ(gate->destructions.load(), 0); // stop keeps the slot for restart.
  EXPECT_TRUE(mixer.ensureSourcesStarted());
  EXPECT_TRUE(gate->capturing.load());
  EXPECT_EQ(gate->starts.load(), 2);
  mixer.stopAll();
  EXPECT_FALSE(gate->capturing.load());
}

TEST_F(AudioMixerTest, ReplacementUnpublishesRetiredSourceBeforeBlockedStop) {
  auto gate = std::make_shared<AudioSourceGate>();
  auto *retired = new GatedAudioSource(gate, false);
  mixer.addSource(std::unique_ptr<IAudioSource>(retired), 0);
  ASSERT_TRUE(mixer.startAll());
  auto *successor = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                       "successor");
  std::atomic<int> calls{0};
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++calls; });
  std::thread replacer([&] {
    mixer.addSource(std::unique_ptr<IAudioSource>(successor), 0);
  });
  const bool entered = gate->waitForEntry();
  EXPECT_TRUE(entered);
  if (entered) {
    std::lock_guard<std::mutex> lock(gate->mutex);
    // addSource has published the successor, but is still stopping the
    // retained old source. Prevent the gate's timeout from destroying the
    // borrowed old pointer while simulating a late PipeWire delivery.
    if (!gate->timedOut.load()) {
      EXPECT_EQ(gate->destructions.load(), 0);
      EXPECT_FALSE(successor->isCapturing());
      const float retiredSamples[4] = {0.9f, 0.9f, 0.9f, 0.9f};
      retired->emitAudio(retiredSamples, 2);
      EXPECT_EQ(calls.load(), 0);
      EXPECT_FLOAT_EQ(mixer.getTrackLevel(0), 0.0f);
    }
  }
  gate->release();
  replacer.join();

  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_EQ(gate->destructions.load(), 1);
  EXPECT_TRUE(successor->isCapturing());
  const float successorSamples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  successor->emitAudio(successorSamples, 2);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_GT(mixer.getTrackLevel(0), 0.0f);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, DrainingPublishedRegistrationRejectsFurtherDelivery) {
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                  "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());
  int retiredCalls = 0;
  auto retired = mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++retiredCalls; });
  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  raw->emitAudio(samples, 2);
  mixer.drainTrackAudioCallback(retired);
  raw->emitAudio(samples, 2);
  EXPECT_EQ(retiredCalls, 1);
  int successorCalls = 0;
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++successorCalls; });
  raw->emitAudio(samples, 2);
  EXPECT_EQ(successorCalls, 1);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, ReplacedSourceSnapshotCannotPolluteSuccessor) {
  auto *oldSource = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                       "old");
  mixer.addSource(std::unique_ptr<IAudioSource>(oldSource), 0);
  ASSERT_TRUE(mixer.startAll());
  // Model a PipeWire thread preempted after loading its callback, but
  // before entering it. No access to oldSource is needed after replacement.
  const auto savedCallback = oldSource->snapshotCallback();
  ASSERT_TRUE(savedCallback);
  auto *successor = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                       "successor");
  mixer.addSource(std::unique_ptr<IAudioSource>(successor), 0);
  ASSERT_TRUE(successor->isCapturing());
  int calls = 0;
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++calls; });
  const float retiredSamples[4] = {0.9f, 0.9f, 0.9f, 0.9f};
  savedCallback(AudioFrame{retiredSamples, 2, 2, 48000, 0});
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(mixer.getTrackLevel(0), 0.0f);

  const float successorSamples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  successor->emitAudio(successorSamples, 2);
  EXPECT_EQ(calls, 1);
  const float successorLevel = mixer.getTrackLevel(0);
  EXPECT_GT(successorLevel, 0.0f);
  // Retired delivery must also leave an already-populated meter unchanged.
  savedCallback(AudioFrame{retiredSamples, 2, 2, 48000, 0});
  EXPECT_EQ(calls, 1);
  EXPECT_FLOAT_EQ(mixer.getTrackLevel(0), successorLevel);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, RemovedSourceSnapshotCannotDeliverOrUpdateMeter) {
  auto *source = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                    "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(source), 0);
  ASSERT_TRUE(mixer.startAll());
  const auto savedCallback = source->snapshotCallback();
  ASSERT_TRUE(savedCallback);
  int calls = 0;
  mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { ++calls; });
  mixer.removeSource(0); // Destroys the source, not the callback snapshot.
  const float samples[4] = {0.9f, 0.9f, 0.9f, 0.9f};
  savedCallback(AudioFrame{samples, 2, 2, 48000, 0});
  EXPECT_EQ(calls, 0);
  EXPECT_FLOAT_EQ(mixer.getTrackLevel(0), 0.0f);
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

// Enter the actual source callback and pause either inside onAudioReceived's
// direct metering signal or inside the output callback. Retirement must drain
// both, even when a fake source's stop() does not drain its producer.
class EnteredCallbackRetirementTest
    : public ::testing::TestWithParam<std::tuple<int, bool>> {};

TEST_P(EnteredCallbackRetirementTest, RetirementWaitsForEnteredCallback) {
  AudioMixer mixer{48000, 2};
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio, "old");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());
  const auto sourceRegistration = AudioMixerTestPeer::sourceCallback(mixer, 0);
  const auto sourceCallback = raw->snapshotCallback();
  auto gate = std::make_shared<AudioSourceGate>();
  const auto [operation, inOutput] = GetParam();
  std::atomic<int> oldCalls{0};
  std::atomic<int> successorCalls{0};
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) {
    ++oldCalls;
    if (inOutput)
      gate->block();
  });
  auto connection = QObject::connect(
      &mixer, &AudioMixer::trackLevelChanged, &mixer,
      [&](int, float) {
        if (!inOutput)
          gate->block();
      }, Qt::DirectConnection);
  std::thread producer([&] {
    const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    sourceCallback(AudioFrame{samples, 2, 2, 48000, 0});
  });
  EXPECT_TRUE(gate->waitForEntry());
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) {
    ++successorCalls;
  });
  std::promise<void> finishedPromise;
  auto finished = finishedPromise.get_future();
  FakeAudioSource *successor = nullptr;
  std::thread retire([&] {
    if (operation == 0) {
      successor = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                      "successor");
      mixer.addSource(std::unique_ptr<IAudioSource>(successor), 0);
    } else if (operation == 1) {
      mixer.removeSource(0);
    } else {
      mixer.stopAll();
    }
    finishedPromise.set_value();
  });
  {
    std::unique_lock<std::mutex> lock(sourceRegistration->mutex);
    EXPECT_TRUE(sourceRegistration->drained.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return sourceRegistration->draining; }));
    EXPECT_EQ(sourceRegistration->inFlight, 1u);
    EXPECT_EQ(finished.wait_for(std::chrono::milliseconds(0)),
              std::future_status::timeout);
  }
  gate->release();
  producer.join();
  retire.join();
  QObject::disconnect(connection);
  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_EQ(oldCalls.load(), inOutput ? 1 : 0);
  EXPECT_EQ(successorCalls.load(), 0); // Old buffer must not reach successor.
  const float samples[4] = {0.9f, 0.9f, 0.9f, 0.9f};
  sourceCallback(AudioFrame{samples, 2, 2, 48000, 0});
  EXPECT_EQ(successorCalls.load(), 0);
  if (operation == 2) {
    ASSERT_TRUE(mixer.startAll());
    raw->emitAudio(samples, 2);
    EXPECT_EQ(successorCalls.load(), 1);
    sourceCallback(AudioFrame{samples, 2, 2, 48000, 0});
    EXPECT_EQ(successorCalls.load(), 1); // Restart never revives old snapshots.
  } else if (successor) {
    successor->emitAudio(samples, 2);
    EXPECT_EQ(successorCalls.load(), 1);
  }
  mixer.stopAll();
}

INSTANTIATE_TEST_SUITE_P(ReplaceRemoveStop, EnteredCallbackRetirementTest,
                        ::testing::Combine(::testing::Values(0, 1, 2),
                                           ::testing::Bool()));

TEST_F(AudioMixerTest, SlowOutputDrainWarnsButDoesNotReleaseLifetimeBarrier) {
  auto *raw = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                 "desktop");
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  ASSERT_TRUE(mixer.startAll());
  auto gate = std::make_shared<AudioSourceGate>();
  auto handle = mixer.setTrackAudioCallback(
      [&](int, const float *, int, int, int) { gate->block(); });
  auto callback = raw->snapshotCallback();
  std::thread producer([&] {
    const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
    callback(AudioFrame{samples, 2, 2, 48000, 0});
  });
  EXPECT_TRUE(gate->waitForEntry());
  std::promise<void> finishedPromise;
  auto finished = finishedPromise.get_future();
  std::thread drainer([&] {
    mixer.drainTrackAudioCallback(handle);
    finishedPromise.set_value();
  });
  {
    // Observe the timeout branch itself, not a sleep near the deadline.
    std::unique_lock<std::mutex> lock(handle->mutex);
    EXPECT_TRUE(handle->drained.wait_for(lock, std::chrono::seconds(4),
                                        [&] { return handle->drainTimedOut; }));
    EXPECT_EQ(handle->inFlight, 1u);
    EXPECT_EQ(finished.wait_for(std::chrono::milliseconds(0)),
              std::future_status::timeout);
  }
  gate->release();
  producer.join();
  drainer.join();
  EXPECT_FALSE(gate->timedOut.load());
  mixer.stopAll();
}

struct StartAttemptState {
  int initializations = 0;
  int starts = 0;
  int stops = 0;
  bool failInitialize = false;
  bool failStart = false;
  IAudioSource::AudioCallback startSnapshot;
};

class FailingStartSource : public FakeAudioSource {
public:
  explicit FailingStartSource(std::shared_ptr<StartAttemptState> state)
      : FakeAudioSource(SourceType::DesktopAudio, "failure"), state(state) {}
  bool initialize(const AudioConfig &) override {
    ++state->initializations;
    return !state->failInitialize;
  }
  bool start() override {
    ++state->starts;
    state->startSnapshot = snapshotCallback();
    FakeAudioSource::start(); // Model a partial start needing cleanup.
    return !state->failStart;
  }
  void stop() override {
    ++state->stops;
    FakeAudioSource::stop();
  }
private:
  std::shared_ptr<StartAttemptState> state;
};

TEST_F(AudioMixerTest, FailedStartsAreStoppedAndCanRetryWithoutRestartingLiveSource) {
  auto state = std::make_shared<StartAttemptState>();
  auto *raw = new FailingStartSource(state);
  mixer.addSource(std::unique_ptr<IAudioSource>(raw), 0);
  auto failedInitializeCallback = raw->snapshotCallback();
  state->failInitialize = true;
  EXPECT_FALSE(mixer.startAll());
  EXPECT_EQ(state->stops, 1);
  EXPECT_EQ(state->starts, 0);
  state->failInitialize = false;
  state->failStart = true;
  EXPECT_FALSE(mixer.ensureSourcesStarted());
  EXPECT_FALSE(raw->isCapturing());
  EXPECT_EQ(state->stops, 2);
  auto failedStartCallback = state->startSnapshot;
  state->failStart = false;
  EXPECT_TRUE(mixer.startAll());
  EXPECT_TRUE(mixer.startAll());
  EXPECT_TRUE(mixer.ensureSourcesStarted());
  EXPECT_EQ(state->initializations, 3);
  EXPECT_EQ(state->starts, 2);
  int calls = 0;
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) { ++calls; });
  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  failedInitializeCallback(AudioFrame{samples, 2, 2, 48000, 0});
  failedStartCallback(AudioFrame{samples, 2, 2, 48000, 0});
  EXPECT_EQ(calls, 0);
  raw->emitAudio(samples, 2);
  EXPECT_EQ(calls, 1);
  mixer.stopAll();
  EXPECT_TRUE(mixer.startAll());
  EXPECT_EQ(state->initializations, 4);
  EXPECT_EQ(state->starts, 3);
  mixer.stopAll();
}

TEST_F(AudioMixerTest, RemovedFailedInitializeCannotDisturbSuccessor) {
  auto gate = std::make_shared<AudioSourceGate>();
  mixer.addSource(std::make_unique<GatedAudioSource>(gate, true, false), 0);
  bool started = true;
  std::thread starter([&] { started = mixer.startAll(); });
  EXPECT_TRUE(gate->waitForEntry());
  auto *successor = new FakeAudioSource(IAudioSource::SourceType::DesktopAudio,
                                       "successor");
  mixer.addSource(std::unique_ptr<IAudioSource>(successor), 0);
  EXPECT_TRUE(successor->isCapturing());
  gate->release();
  starter.join();
  EXPECT_FALSE(started);
  EXPECT_FALSE(gate->timedOut.load());
  EXPECT_EQ(gate->starts.load(), 0);
  EXPECT_EQ(gate->stops.load(), 2);
  EXPECT_EQ(gate->destructions.load(), 1);
  EXPECT_TRUE(successor->isCapturing());
  int calls = 0;
  mixer.setTrackAudioCallback([&](int, const float *, int, int, int) { ++calls; });
  const float samples[4] = {0.1f, 0.1f, 0.1f, 0.1f};
  successor->emitAudio(samples, 2);
  EXPECT_EQ(calls, 1);
  mixer.stopAll();
}

#include "test_AudioMixer.moc"
