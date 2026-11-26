/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/RecordingEngine.h"
#include "core/RecordingSession.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/IEncoder.h"
#include "settings/SettingsManager.h"
#include "DetectionFixture.h"
#include "audio/AudioMixer.h"
#include <QCoreApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>
#include <array>
#include <condition_variable>
#include <mutex>

using namespace TrophyBackend;
namespace {
class Source final : public IVideoCaptureSource {
public:
  QString label;
  uint32_t pid = 0;
  bool autoReady = true;
  CaptureConfig config;
  CaptureErrorKind errorKind = CaptureErrorKind::Other;
  explicit Source(int &destroyed) : m_destroyed(destroyed) {}
  ~Source() override { ++m_destroyed; }
  bool initialize(const CaptureConfig &value) override { config = value; return true; }
  void setFrameCallback(FrameCallback) override {}
  bool startCapture() override {
    if (autoReady) QTimer::singleShot(0, this, [this] { emit ready(); });
    return true;
  }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {640, 480}; }
  QString name() const override { return QStringLiteral("Test window"); }
  QString sourceLabel() const override { return label; }
  uint32_t sourcePid() const override { return pid; }
  CaptureErrorKind lastErrorKind() const override { return errorKind; }
private:
  int &m_destroyed;
};
class Encoder final : public IEncoder {
public:
  bool initialize(const EncoderConfig &) override { return true; }
  bool encodeFrame(const VideoFrame &, EncodedPacket &) override { return false; }
  int flush(std::vector<EncodedPacket> &) override { return 0; }
  EncoderConfig config() const override { return {}; }
  EncoderType type() const override { return EncoderType::Software_x264; }
  QString name() const override { return QStringLiteral("test encoder"); }
  bool isHardwareEncoder() const override { return false; }
  std::vector<uint8_t> extraData() const override { return {}; }
  int codecId() const override { return 27; }
  int encodedPixelFormat() const override { return 0; }
};
class Output final : public EncodedOutput {
public:
  std::atomic<int> begins{0}, writes{0}, ends{0};
  bool failWrite = false, failEnd = false;
  EncodedFrame last;
  EncodedStream format;
  bool begin(const EncodedStream &value) override { format = value; ++begins; return true; }
  bool write(const EncodedFrame &packet) override { ++writes; last = packet; return !failWrite; }
  bool end() override { ++ends; return !failEnd; }
  bool started() const override { return writes > 0; }
  QString error() const override { return QStringLiteral("test output error"); }
  void setAudioHistory(quint64, const QJsonObject &) override {}
};
// Gates always release before the engine is destroyed, including ASSERT exits.
struct LookupGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, released = false;
  void block() {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [&] { return released; });
  }
  bool wait() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(3), [&] { return entered; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    condition.notify_all();
  }
};
class RoleSource final : public IAudioSource {
public:
  RoleSource(SourceType type, bool &destroyed) : m_type(type), m_destroyed(destroyed) {}
  ~RoleSource() override { m_destroyed = true; }
  bool initialize(const AudioConfig &) override { return true; }
  void setCallback(AudioCallback) override {}
  bool start() override { return true; }
  void stop() override {}
  bool isCapturing() const override { return true; }
  SourceType sourceType() const override { return m_type; }
  QString displayName() const override { return "role fixture"; }
  QString identifier() const override { return "role fixture"; }
  float peakLevel() const override { return 0; }
  void setVolume(float) override {}
  float volume() const override { return 1; }
private:
  SourceType m_type;
  bool &m_destroyed;
};
}
namespace TrophyBackend {
struct RecordingEngineTestAccess {
  using Preparation = RecordingEngine::AudioPreparation;
  static auto snapshot(const RecordingEngine &engine) { return engine.snapshotAudioPreparation(); }
  static auto plan(const Preparation &input) { return RecordingEngine::planAudioRouting(input); }
  static void lookup(RecordingEngine &engine, std::function<Preparation(Preparation)> lookup) {
    engine.m_audioPreparationLookup = std::move(lookup);
  }
  static auto prepared(const RecordingEngine &engine) { return engine.m_preparedAudio; }
  static void observeBuild(RecordingEngine &engine, std::function<void()> observe) {
    const auto build = engine.m_pipelineBuilder;
    engine.m_pipelineBuilder = [build, observe] { observe(); return build(); };
  }
  static void source(RecordingEngine &engine, int &starts, int &destroyed,
                     QPointer<Source> &current, QString label = {}, uint32_t pid = 0,
                     bool autoReady = true) {
    engine.m_captureFactory = [&starts, &destroyed, &current, label, pid, autoReady] {
      ++starts;
      auto source = std::make_unique<Source>(destroyed);
      source->label = label;
      source->pid = pid;
      source->autoReady = autoReady;
      current = source.get();
      return source;
    };
  }
  static void shortenRecovery(RecordingEngine &engine) {
    engine.m_captureEndGraceMs = 1;
    engine.m_recoveryDelayMs = 30;
  }
  static bool awaitingCensus(const RecordingEngine &engine) {
    return engine.recoveryAwaitingCensus();
  }
  static bool currentInstance(const RecordingEngine &engine) {
    return engine.recoveryInstanceIsCurrent();
  }
  static void dismissResume(RecordingEngine &engine) { engine.stopRecording(); }
  static bool lookupPending(const RecordingEngine &engine) {
    return engine.m_audioPreparationWorker.valid();
  }
  static void lateVoice(RecordingEngine &engine, AudioMixer &mixer) {
    engine.m_session = std::make_unique<RecordingSession>(
        "unused", nullptr, nullptr, nullptr, &mixer, false, 128, 3);
    engine.m_session->setVoiceTrack(2);
    engine.m_session->markSourceConnected(2);
    engine.setState(RecordingEngine::RecordingState::Recording);
  }
  static void install(RecordingEngine &engine, int &starts, int &destroyed) {
    auto &capture = engine;
    capture.m_captureFactory = [&] { ++starts; return std::make_unique<Source>(destroyed); };
    capture.m_audioPreparationLookup = [](auto input) { return input; };
    capture.m_pipelineBuilder = [&capture] {
      capture.m_videoEncoder = std::make_unique<Encoder>();
      capture.m_muxer = capture.m_muxerFactory();
      capture.m_muxer->initialize({});
      StreamInfo video{};
      video.type = StreamInfo::Type::Video;
      video.codecId = 27;
      video.width = 640;
      video.height = 480;
      video.fps = 30;
      capture.m_muxer->addStream(video);
      return capture.m_muxer->writeHeader();
    };
  }
  static void packet(RecordingEngine &engine, int pts) {
    EncodedPacket packet;
    packet.pts = packet.dts = pts;
    packet.duration = 33;
    packet.isKeyframe = true;
    packet.data = {1, 2, 3};
    ASSERT_TRUE(engine.m_muxer);
    ASSERT_TRUE(engine.m_muxer->writePacket(packet));
  }
  static const void *encoder(RecordingEngine &engine) { return engine.m_videoEncoder.get(); }
  static void reconcile(RecordingEngine &engine) { engine.reconcile(); }
};
}
class RecordingEngineTest : public ::testing::Test {
protected:
  QTemporaryDir directory;
  QByteArray oldConfig;
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "packet-engine-tests";
      static char *argv[] = {name, nullptr};
      new QCoreApplication(argc, argv);
    }
    oldConfig = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
  }
  void TearDown() override {
    if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME");
    else qputenv("XDG_CONFIG_HOME", oldConfig);
  }
};
TEST_F(RecordingEngineTest, NoDemandNeverStartsCapture) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  QCoreApplication::processEvents();
  EXPECT_EQ(starts, 0);
  EXPECT_FALSE(engine.hasDemand());
}
TEST_F(RecordingEngineTest, AttachSwitchAndLastDetachUseOneEncoder) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  auto first = std::make_shared<Output>(), second = std::make_shared<Output>();
  const auto a = engine.attach(first, {});
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  const auto encoder = RecordingEngineTestAccess::encoder(engine);
  RecordingEngineTestAccess::packet(engine, 0);
  const auto b = engine.attach(second, {});
  engine.detach(a);
  ASSERT_TRUE(QTest::qWaitFor([&] { return first->ends == 1; }));
  RecordingEngineTestAccess::packet(engine, 33);
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(RecordingEngineTestAccess::encoder(engine), encoder);
  EXPECT_EQ(first->writes.load(), 1);
  EXPECT_EQ(second->writes.load(), 1);
  EXPECT_TRUE(engine.hasDemand());
  engine.detach(b);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
  EXPECT_FALSE(engine.hasDemand());
  EXPECT_EQ(destroyed, 1);
  EXPECT_EQ(second->ends.load(), 1);
}
TEST_F(RecordingEngineTest, OutputFailureDoesNotStopSurvivingConsumer) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  auto healthy = std::make_shared<Output>(), failed = std::make_shared<Output>();
  const auto a = engine.attach(healthy, {});
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  failed->failWrite = true;
  engine.attach(failed, {});
  QSignalSpy errors(&engine, &RecordingEngine::outputError);
  RecordingEngineTestAccess::packet(engine, 0);
  ASSERT_TRUE(QTest::qWaitFor([&] { return errors.size() == 1; }));
  RecordingEngineTestAccess::packet(engine, 33);
  EXPECT_EQ(healthy->writes.load(), 2);
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(engine.state(), RecordingEngine::State::Running);
  engine.detach(a);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
TEST_F(RecordingEngineTest, SameTurnReplacementCancelsLastConsumerShutdown) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  const auto a = engine.attach(std::make_shared<Output>(), {});
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  engine.detach(a);
  const auto b = engine.attach(std::make_shared<Output>(), {});
  QCoreApplication::processEvents();
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(destroyed, 0);
  EXPECT_EQ(engine.state(), RecordingEngine::State::Running);
  engine.detach(b);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
TEST_F(RecordingEngineTest, ConsumerSwitchWhileStartingRetainsPendingCapture) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  const auto a = engine.attach(std::make_shared<Output>(), {});
  RecordingEngineTestAccess::reconcile(engine);
  ASSERT_EQ(engine.state(), RecordingEngine::State::Starting);
  const auto b = engine.attach(std::make_shared<Output>(), {});
  engine.detach(a);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(destroyed, 0);
  engine.detach(b);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}

class EngineNamingTest : public RecordingEngineTest,
                         public ::testing::WithParamInterface<std::tuple<QString, QString, QString>> {};
TEST_P(EngineNamingTest, SelectedSourceNamesStreamWithoutBorrowingLaterDetection) {
  const auto [target, label, expected] = GetParam();
  SettingsManager settings;
  GameDetectionService detector;
  detector.addToWhitelist("steam_app_42424242", "Detected game");
  DetectionFixture::publish(detector, QCoreApplication::applicationPid(), "steam_app_42424242");
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, label);
  auto output = std::make_shared<Output>();
  const auto id = engine.attach(output, target);
  ASSERT_NE(id, 0u);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  EXPECT_EQ(engine.currentGame(), expected);
  RecordingEngineTestAccess::packet(engine, 0);
  EXPECT_EQ(output->format.game, expected);
  if (target.isEmpty()) {
    EXPECT_TRUE(output->format.identity.isEmpty());
    EXPECT_EQ(engine.capturedSteamAppId(), 0u);
    EXPECT_EQ(RecordingEngineTestAccess::snapshot(engine).pid, 0u);
    EXPECT_TRUE(RecordingEngineTestAccess::snapshot(engine).hints.isEmpty());
    EXPECT_FALSE(capture->config.windowOnly);
  } else {
    EXPECT_TRUE(capture->config.windowOnly);
  }
  engine.detach(id);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
INSTANTIATE_TEST_SUITE_P(SourceLabels, EngineNamingTest, ::testing::Values(
    std::make_tuple(QString(), QString("Firefox"), QString("Firefox")),
    std::make_tuple(QString(), QString(), QString("Manual Recording")),
    std::make_tuple(QString(), QString("kwin_wayland"), QString("Manual Recording")),
    std::make_tuple(QString(), QString("org.gnome.Shell"), QString("Manual Recording")),
    std::make_tuple(QString(), QString("steam_app_42424242"), QString("Manual Recording")),
    std::make_tuple(QString("Original target"), QString(), QString("Original target")),
    std::make_tuple(QString("Original target"), QString("kwin_wayland"), QString("Original target")),
    std::make_tuple(QString("Detected game"), QString("Firefox"), QString("Firefox")),
    std::make_tuple(QString("Detected game"), QString("detected game.exe"), QString("Detected game"))));

class EngineAudioIdentityTest : public RecordingEngineTest,
                                public ::testing::WithParamInterface<int> {};
TEST_P(EngineAudioIdentityTest, PreparationPinsTargetAndManualSuccessorClearsIdentity) {
  const int scenario = GetParam();
  SettingsManager settings;
  settings.setAudioMode("game");
  GameDetectionService detector;
  detector.addToWhitelist("steam_app_42424242", "Pinned game");
  detector.addToWhitelist("steam_app_42424243", scenario == 3 ? "Another game" : "Pinned game");
  const auto pid = QCoreApplication::applicationPid();
  DetectionFixture::publish(detector, pid, "steam_app_42424242");
  const auto original = detector.captureIdentityInfo("Pinned game");
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, {}, 0, false);
  const QString target = scenario == 4 ? QString() : scenario == 5 ? "Other target" : "Pinned game";
  auto output = std::make_shared<Output>();
  const auto id = engine.attach(output, target);
  ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.isNull(); }));
  if (scenario == 1) capture->pid = 2147483647; // Portal metadata must not override the pin.
  if (scenario == 2 || scenario == 3)
    DetectionFixture::publish(detector, pid, "steam_app_42424243");
  const auto expectedPid = scenario >= 4 ? 0u : uint32_t(pid);
  emit capture->ready();
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  const auto prepared = RecordingEngineTestAccess::prepared(engine);
  EXPECT_EQ(prepared.pid, expectedPid);
  if (scenario == 4) {
    EXPECT_TRUE(prepared.hints.isEmpty());
  } else {
    EXPECT_TRUE(prepared.hints.contains(target));
    EXPECT_FALSE(prepared.hints.contains("Another game"));
  }
  if (scenario < 4) {
    EXPECT_TRUE(prepared.hints.contains("trophy-instance:" + original["detectedGameInstance"].toString()));
  }
  EXPECT_EQ(RecordingEngineTestAccess::snapshot(engine).pid, expectedPid);
  engine.detach(id);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
  const auto manual = engine.attach(std::make_shared<Output>(), {});
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Starting; }));
  EXPECT_EQ(RecordingEngineTestAccess::snapshot(engine).pid, 0u);
  EXPECT_TRUE(RecordingEngineTestAccess::snapshot(engine).hints.isEmpty());
  engine.detach(manual);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
INSTANTIATE_TEST_SUITE_P(PinnedInputs, EngineAudioIdentityTest, ::testing::Range(0, 6));

class EngineStartupTest : public RecordingEngineTest,
                          public ::testing::WithParamInterface<int> {};
TEST_P(EngineStartupTest, DelayedLookupCannotStartCancelledOrObsoleteInputs) {
  // 0: settings change, 1: stop only, 2: stop and a different target.
  const int scenario = GetParam();
  SettingsManager settings;
  settings.setAudioMode("game");
  settings.setVoiceChatApp("old-voice");
  int starts = 0, destroyed = 0, builds = 0;
  std::atomic<int> lookups{0};
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  auto gate = std::make_shared<LookupGate>();
  const auto release = qScopeGuard([&] { gate->release(); });
  RecordingEngineTestAccess::lookup(engine, [gate, &lookups](auto input) {
    if (++lookups == 1) {
      EXPECT_EQ(input.voiceApp, "old-voice");
      gate->block();
    }
    return input;
  });
  RecordingEngineTestAccess::observeBuild(engine, [&] {
    ++builds;
    const auto prepared = RecordingEngineTestAccess::prepared(engine);
    EXPECT_EQ(prepared.voiceApp, "new-voice");
    EXPECT_EQ(prepared.micDevice, "new-mic");
    EXPECT_EQ(prepared.gameDevice, "new-game");
    EXPECT_EQ(prepared.mode, "desktop");
    EXPECT_FALSE(prepared.separate);
  });
  const auto first = engine.attach(std::make_shared<Output>(), "Old target");
  ASSERT_TRUE(QTest::qWaitFor([&] { return lookups.load() == 1; }));
  ASSERT_TRUE(gate->wait());
  bool heartbeat = false;
  QTimer::singleShot(0, &engine, [&] { heartbeat = true; });
  QCoreApplication::processEvents();
  EXPECT_TRUE(heartbeat);
  EXPECT_EQ(builds, 0);
  if (scenario != 0) {
    engine.detach(first);
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
  }
  settings.setVoiceChatApp("new-voice");
  settings.setMicrophoneDevice("new-mic");
  settings.setGameAudioDevice("new-game");
  settings.setAudioMode("desktop");
  settings.setSeparateAudioTracks(false);
  quint64 replacement = 0;
  if (scenario == 2) {
    replacement = engine.attach(std::make_shared<Output>(), "Replacement target");
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Starting; }));
  }
  EXPECT_EQ(lookups.load(), 1); // No overlapping preparation workers.
  gate->release();
  if (scenario == 1) {
    ASSERT_TRUE(QTest::qWaitFor([&] { return !RecordingEngineTestAccess::lookupPending(engine); }));
    EXPECT_EQ(engine.state(), RecordingEngine::State::Idle);
    EXPECT_FALSE(engine.hasDemand());
    EXPECT_EQ(builds, 0);
    EXPECT_EQ(lookups.load(), 1);
  } else {
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
    EXPECT_EQ(engine.currentGame(), scenario == 2 ? "Replacement target" : "Old target");
    EXPECT_EQ(lookups.load(), 2);
    EXPECT_EQ(builds, 1);
    engine.detach(scenario == 2 ? replacement : first);
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
  }
}
INSTANTIATE_TEST_SUITE_P(DelayedPreparation, EngineStartupTest, ::testing::Range(0, 3));

class EngineRoutingTest : public RecordingEngineTest,
                          public ::testing::WithParamInterface<std::tuple<bool, bool, bool, bool>> {};
TEST_P(EngineRoutingTest, StartupLayoutAvoidsVoiceDuplicationDuringDesktopFallback) {
  const auto [separate, gameFound, voiceEnabled, voiceFound] = GetParam();
  RecordingEngineTestAccess::Preparation input;
  input.mode = "game";
  input.separate = separate;
  input.gameNode = gameFound ? 42 : 0xffffffffu;
  input.voiceApp = voiceEnabled ? "discord" : "";
  input.voiceNode = voiceFound ? 43 : 0xffffffffu;
  const auto routing = RecordingEngineTestAccess::plan(input);
  const bool dedicatedVoice = gameFound && voiceEnabled && voiceFound;
  EXPECT_EQ(routing.gameFound, gameFound);
  EXPECT_EQ(routing.gamePending, !gameFound);
  EXPECT_EQ(routing.voiceFound, dedicatedVoice);
  EXPECT_EQ(routing.voiceTrack, voiceEnabled && (separate || dedicatedVoice) ? 1 : -1);
  EXPECT_EQ(routing.micTrack, separate || dedicatedVoice ? 2 : 1);
  EXPECT_EQ(routing.trackCount, separate || dedicatedVoice ? 3 : 2);
  EXPECT_NE(routing.micTrack, routing.voiceTrack);
  // Desktop audio already contains Voice, even when a separate node was found.
  if (!gameFound) {
    EXPECT_FALSE(routing.voiceFound);
  }
  input.mode = "desktop";
  const auto desktop = RecordingEngineTestAccess::plan(input);
  EXPECT_EQ(desktop.trackCount, 2);
  EXPECT_EQ(desktop.micTrack, 1);
  EXPECT_EQ(desktop.voiceTrack, -1);
  EXPECT_FALSE(desktop.voiceFound);
  EXPECT_FALSE(desktop.gamePending);
}
INSTANTIATE_TEST_SUITE_P(AudioPermutations, EngineRoutingTest,
    ::testing::Combine(::testing::Bool(), ::testing::Bool(), ::testing::Bool(), ::testing::Bool()));

TEST_F(RecordingEngineTest, DisablingLateVoiceRetiresActualSlotAndPreservesMicrophone) {
  SettingsManager settings;
  settings.setAudioMode("game");
  settings.setSeparateAudioTracks(false);
  settings.setVoiceChatApp("");
  bool micDestroyed = false, voiceDestroyed = false;
  AudioMixer mixer(48000, 2);
  mixer.addSource(std::make_unique<RoleSource>(IAudioSource::SourceType::Microphone, micDestroyed), 1);
  mixer.addSource(std::make_unique<RoleSource>(IAudioSource::SourceType::ApplicationAudio, voiceDestroyed), 2);
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setAudioMixer(&mixer);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  engine.attach(std::make_shared<Output>(), {});
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  RecordingEngineTestAccess::lateVoice(engine, mixer);
  engine.applyVoiceChatApp();
  ASSERT_TRUE(QTest::qWaitFor([&] { return !mixer.sourceOperationPending(2); }));
  EXPECT_TRUE(voiceDestroyed);
  EXPECT_FALSE(micDestroyed);
  EXPECT_TRUE(mixer.sourceIsCapturing(1));
}

class RestoreCapabilityTest : public RecordingEngineTest,
                              public ::testing::WithParamInterface<int> {};
TEST_P(RestoreCapabilityTest, RevocationIsScopedToTheOriginatingAttemptAndToken) {
  const int scenario = GetParam(); // Identity key, legacy name, newer token, old source.
  SettingsManager settings;
  const QString key = "steam_app_42424242", name = "Same name";
  settings.setRestoreToken(scenario == 1 ? name : key, "old-token");
  settings.setRestoreToken("steam_app_42424243", "other-game-token");
  if (scenario == 2) settings.setRestoreToken(key, "new-token");
  int destroyed = 0, stale = 0;
  Source source(destroyed);
  CaptureLifecycle lifecycle;
  lifecycle.configure(nullptr, &settings);
  lifecycle.bindSource(&source, name, key, false,
      [scenario] { return scenario != 3; }, [] {}, [](const QString &) {},
      [&](const QString &) { ++stale; });
  emit source.restoreTokenStale("old-token");
  EXPECT_EQ(stale, scenario == 3 ? 0 : 1);
  EXPECT_EQ(settings.getRestoreTokenForIdentity(key, name),
            scenario == 2 ? "new-token" : scenario == 3 ? "old-token" : "");
  EXPECT_EQ(settings.getRestoreToken("steam_app_42424243"), "other-game-token");
}
INSTANTIATE_TEST_SUITE_P(AttemptAndTokenIdentity, RestoreCapabilityTest, ::testing::Range(0, 4));

class EngineRestoreAuthorizationTest : public RecordingEngineTest,
                                       public ::testing::WithParamInterface<bool> {};
TEST_P(EngineRestoreAuthorizationTest, RejectedRestoreNotifiesConsumerAndWaitsForAuthorization) {
  const bool recovery = GetParam();
  const QString key = "steam_app_42424242", name = "Restore fixture";
  SettingsManager settings;
  settings.setRestoreTokenForIdentity(key, name, "rejected-token");
  GameDetectionService detector;
  detector.addToWhitelist(key, name);
  DetectionFixture::publish(detector, QCoreApplication::applicationPid(), key);
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, {}, 0, false);
  RecordingEngineTestAccess::shortenRecovery(engine);
  auto output = std::make_shared<Output>();
  const auto id = engine.attach(output, name);
  QSignalSpy authorization(&engine, &RecordingEngine::restoreAuthNeeded);
  QSignalSpy stopped(&engine, &RecordingEngine::outputStopped);
  ASSERT_TRUE(QTest::qWaitFor([&] { return capture && starts == 1; }));
  if (recovery) {
    emit capture->ready();
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
    RecordingEngineTestAccess::packet(engine, 0);
    capture->errorKind = IVideoCaptureSource::CaptureErrorKind::SourceDestroyed;
    emit capture->captureError("captured window destroyed");
    ASSERT_TRUE(QTest::qWaitFor([&] { return starts == 2 && capture; }));
  }
  ASSERT_EQ(capture->config.waylandRestoreToken, "rejected-token");
  emit capture->restoreTokenStale("rejected-token");
  EXPECT_TRUE(settings.getRestoreTokenForIdentity(key, name).isEmpty());
  ASSERT_TRUE(QTest::qWaitFor([&] { return stopped.size() == 1; }));
  ASSERT_EQ(authorization.size(), 1);
  EXPECT_EQ(authorization.first()[0].toULongLong(), id);
  EXPECT_EQ(authorization.first()[1].toString(), name);
  EXPECT_EQ(authorization.first()[2].toString(), key);
  QTest::qWait(100);
  EXPECT_EQ(starts, recovery ? 2 : 1);
  EXPECT_EQ(engine.state(), RecordingEngine::State::Idle);
  EXPECT_FALSE(engine.hasDemand());
}
INSTANTIATE_TEST_SUITE_P(InitialAndRecovery, EngineRestoreAuthorizationTest, ::testing::Bool());

class EngineCensusTest : public RecordingEngineTest,
                         public ::testing::WithParamInterface<int> {};
TEST_P(EngineCensusTest, LauncherLossWaitsOnlyForApprovedExactInstance) {
  // Successor, no payload, explicit stop, revoke, ambiguous loss, namesake, runtime exit.
  const int scenario = GetParam();
  QProcess bootstrap, runtime, successor;
  const auto cleanup = qScopeGuard([&] {
    for (auto *process : {&bootstrap, &runtime, &successor})
      if (process->state() != QProcess::NotRunning) {
        process->terminate();
        process->waitForFinished(3000);
      }
  });
  for (auto *process : {&bootstrap, &runtime, &successor}) {
    process->start("sleep", {"30"});
    ASSERT_TRUE(process->waitForStarted());
  }
  const QString key = "steam_app_42424242", name = "Launcher fixture";
  SettingsManager settings;
  settings.setRestoreTokenForIdentity(key, name, "approved-token");
  GameDetectionService detector;
  detector.addToWhitelist(key, name);
  DetectionFixture::publish(detector, bootstrap.processId(), key);
  DetectionFixture::setLaunchRuntime(detector, runtime.processId());
  const auto original = detector.captureIdentityInfo(name)["detectedGameInstance"].toString();
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture);
  RecordingEngineTestAccess::shortenRecovery(engine);
  auto output = std::make_shared<Output>();
  const auto id = engine.attach(output, name);
  QSignalSpy ended(&engine, &RecordingEngine::generationEnded);
  QSignalSpy errors(&engine, &RecordingEngine::outputError);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  RecordingEngineTestAccess::packet(engine, 0);
  EXPECT_EQ(output->writes.load(), 1);
  bootstrap.terminate();
  ASSERT_TRUE(bootstrap.waitForFinished());
  ASSERT_FALSE(detector.instanceAlive(original));
  ASSERT_TRUE(detector.instanceLaunchContextAlive(original));
  capture->errorKind = scenario == 4 ? IVideoCaptureSource::CaptureErrorKind::SourceUnavailable
                                    : IVideoCaptureSource::CaptureErrorKind::SourceDestroyed;
  emit capture->captureError("launcher window disappeared before census");
  if (scenario != 4) {
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Recovering; }));
    QTest::qWait(40); // Deliberately beyond the 1ms classification grace.
    EXPECT_EQ(ended.size(), 0);
    EXPECT_EQ(errors.size(), 0);
    EXPECT_TRUE(RecordingEngineTestAccess::awaitingCensus(engine));
    switch (scenario) {
    case 0: DetectionFixture::promotePayload(detector, successor.processId()); break;
    case 1: DetectionFixture::clear(detector); break;
    case 2: engine.detach(id); break;
    case 3: settings.removeRestoreToken(key); break;
    case 5: DetectionFixture::publish(detector, successor.processId(), key); break;
    case 6:
      runtime.terminate();
      ASSERT_TRUE(runtime.waitForFinished());
      break;
    }
  }
  ASSERT_TRUE(QTest::qWaitFor([&] { return !ended.isEmpty(); }));
  EXPECT_TRUE(ended.first()[1].toBool());
  EXPECT_EQ(ended.first()[2].toBool(), scenario == 0);
  EXPECT_EQ(errors.size(), 0);
  EXPECT_EQ(output->ends.load(), 1);
  if (scenario == 0) {
    ASSERT_TRUE(QTest::qWaitFor([&] {
      return starts == 2 && engine.state() == RecordingEngine::State::Running;
    }));
    EXPECT_EQ(capture->config.waylandRestoreToken, "approved-token");
    EXPECT_EQ(RecordingEngineTestAccess::prepared(engine).pid, uint32_t(successor.processId()));
    EXPECT_EQ(detector.captureIdentityInfo(name)["detectedGameInstance"].toString(), original);
    RecordingEngineTestAccess::packet(engine, 33);
    EXPECT_EQ(output->begins.load(), 2);
    EXPECT_EQ(output->writes.load(), 2); // Same consumer intent, new generation.
    engine.detach(id);
  }
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return engine.state() == RecordingEngine::State::Idle && !engine.hasDemand();
  }));
  QTest::qWait(80);
  EXPECT_EQ(starts, scenario == 0 ? 2 : 1);
  EXPECT_EQ(errors.size(), 0);
}
INSTANTIATE_TEST_SUITE_P(CensusOutcomes, EngineCensusTest, ::testing::Range(0, 7),
    ([](const ::testing::TestParamInfo<int> &info) {
      return std::array{"VerifiedSuccessor", "NoPayload", "ExplicitStop", "RevokedApproval",
                        "AmbiguousLoss", "Namesake", "RuntimeExit"}[info.param];
    }));

class EngineIdentityFenceTest : public RecordingEngineTest,
                                public ::testing::WithParamInterface<int> {};
TEST_P(EngineIdentityFenceTest, RecoveryRejectsStaleOrUnselectedMembership) {
  // Same live PID is insufficient: start time and executable/payload must agree.
  const int scenario = GetParam();
  QProcess other;
  if (scenario == 2)
    other.start("python3", {"-c", "import time; time.sleep(30)"});
  else
    other.start("sleep", {"30"});
  ASSERT_TRUE(other.waitForStarted());
  const auto cleanup = qScopeGuard([&] { other.terminate(); other.waitForFinished(3000); });
  SettingsManager settings;
  const QString key = "steam_app_42424242", name = "Identity fixture";
  settings.setRestoreTokenForIdentity(key, name, "approved-token");
  GameDetectionService detector;
  detector.addToWhitelist(key, name);
  const auto pinnedPid = scenario == 2 ? other.processId() : QCoreApplication::applicationPid();
  DetectionFixture::publish(detector, pinnedPid, key);
  const auto original = detector.captureIdentityInfo(name)["detectedGameInstance"].toString();
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  const auto id = engine.attach(std::make_shared<Output>(), name);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  ASSERT_TRUE(RecordingEngineTestAccess::currentInstance(engine));
  auto process = ProcessCensus::inspect(pinnedPid);
  if (scenario == 0) {
    ++process.start; // Models a stale census record after PID reuse.
    DetectionFixture::replaceProcessRecord(detector, process);
  } else if (scenario == 1) {
    process.executable = "/unrelated/helper";
    DetectionFixture::replaceProcessRecord(detector, process);
  } else if (scenario == 2) {
    const auto originalPayload = process.payload();
    process.arguments = {"python3", "/unrelated/script.py"};
    ASSERT_NE(process.payload(), originalPayload);
    DetectionFixture::replaceProcessRecord(detector, process);
  } else if (scenario == 3 || scenario == 4) {
    DetectionFixture::addIndependentInstance(detector, other.processId(), key, scenario == 4);
    EXPECT_TRUE(detector.instanceAlive(original)); // Both instances are independently alive.
  } else {
    detector.addToBlacklist(key);
  }
  EXPECT_EQ(RecordingEngineTestAccess::currentInstance(engine), scenario == 3);
  if (scenario != 5) {
    EXPECT_EQ(engine.canReuseCapture(name), scenario == 3);
  }
  engine.detach(id);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
INSTANTIATE_TEST_SUITE_P(IdentityFences, EngineIdentityFenceTest, ::testing::Range(0, 6),
    ([](const ::testing::TestParamInfo<int> &info) {
      return std::array{"ReusedPid", "ChangedExecutable", "ChangedPayload",
                        "ConcurrentUnselectedInstance", "SelectedNamesake", "Denied"}[info.param];
    }));

class EngineStaleCallbackTest : public RecordingEngineTest,
                               public ::testing::WithParamInterface<bool> {};
TEST_P(EngineStaleCallbackTest, QueuedOldSourceCannotStartOrCancelReplacement) {
  SettingsManager settings;
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, {}, 0, false);
  const auto first = engine.attach(std::make_shared<Output>(), "Old target");
  RecordingEngineTestAccess::reconcile(engine);
  ASSERT_FALSE(capture.isNull());
  if (GetParam())
    emit capture->captureError("late error from retired source");
  else
    emit capture->ready();
  engine.detach(first);
  const auto replacement = engine.attach(std::make_shared<Output>(), "New target");
  RecordingEngineTestAccess::reconcile(engine); // Invalidate the old generation before delivery.
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return starts == 2 && engine.state() == RecordingEngine::State::Starting;
  }));
  QSignalSpy errors(&engine, &RecordingEngine::outputError);
  QTest::qWait(40);
  EXPECT_EQ(engine.state(), RecordingEngine::State::Starting);
  emit capture->ready();
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  EXPECT_EQ(engine.currentGame(), "New target");
  EXPECT_EQ(errors.size(), 0);
  engine.detach(replacement);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}
INSTANTIATE_TEST_SUITE_P(StaleDelivery, EngineStaleCallbackTest, ::testing::Bool());

TEST_F(RecordingEngineTest, RestoreTokenUpdateRetainsOriginatingIdentityAfterSelectionChanges) {
  SettingsManager settings;
  GameDetectionService detector;
  detector.addToWhitelist("steam_app_42424242", "Origin game");
  detector.addToWhitelist("steam_app_42424243", "Other game");
  const auto pid = QCoreApplication::applicationPid();
  DetectionFixture::publish(detector, pid, "steam_app_42424242");
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, {}, 0, false);
  const auto id = engine.attach(std::make_shared<Output>(), "Origin game");
  ASSERT_TRUE(QTest::qWaitFor([&] { return !capture.isNull(); }));
  DetectionFixture::publish(detector, pid, "steam_app_42424243");
  emit capture->restoreTokenUpdated("origin-token");
  EXPECT_EQ(settings.getRestoreTokenForIdentity("steam_app_42424242", "Origin game"), "origin-token");
  EXPECT_TRUE(settings.getRestoreTokenForIdentity("steam_app_42424243", "Other game").isEmpty());
  engine.detach(id);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
}

class EnginePortalFenceTest : public RecordingEngineTest,
                              public ::testing::WithParamInterface<int> {};
TEST_P(EnginePortalFenceTest, ClosedGrantNeverAutomaticallyReopensAcrossCensusOrSelection) {
  // Live owner, successor, ended owner, manual, dismiss, namesake, cancel, empty, selection switch.
  const int scenario = GetParam();
  QProcess bootstrap, runtime, successor;
  const auto cleanup = qScopeGuard([&] {
    for (auto *process : {&bootstrap, &runtime, &successor})
      if (process->state() != QProcess::NotRunning) {
        process->terminate();
        process->waitForFinished(3000);
      }
  });
  for (auto *process : {&bootstrap, &runtime, &successor}) {
    process->start("sleep", {"30"});
    ASSERT_TRUE(process->waitForStarted());
  }
  const QString key = "steam_app_42424242", name = "Portal fixture";
  SettingsManager settings;
  settings.setRestoreTokenForIdentity(key, name, "existing-token");
  GameDetectionService detector;
  detector.addToWhitelist(key, name);
  detector.addToWhitelist("exe:/other", "Other game");
  DetectionFixture::publish(detector, bootstrap.processId(), key);
  DetectionFixture::setLaunchRuntime(detector, runtime.processId());
  const auto original = detector.captureIdentityInfo(name)["detectedGameInstance"].toString();
  const auto clear = qScopeGuard([&] { DetectionFixture::clear(detector); });
  int starts = 0, destroyed = 0;
  QPointer<Source> capture;
  RecordingEngine engine;
  engine.setSettingsManager(&settings);
  engine.setGameDetectionService(&detector);
  RecordingEngineTestAccess::install(engine, starts, destroyed);
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture);
  RecordingEngineTestAccess::shortenRecovery(engine);
  auto output = std::make_shared<Output>();
  engine.attach(output, scenario == 3 ? QString() : name);
  QSignalSpy ended(&engine, &RecordingEngine::generationEnded);
  QSignalSpy errors(&engine, &RecordingEngine::outputError);
  ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
  RecordingEngineTestAccess::packet(engine, 0);
  if (scenario == 1 || scenario == 2) {
    bootstrap.terminate();
    ASSERT_TRUE(bootstrap.waitForFinished());
  }
  capture->errorKind = IVideoCaptureSource::CaptureErrorKind::SessionClosed;
  emit capture->captureError("portal sharing ended");
  if (scenario == 1 || scenario == 2) {
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Stopping; }));
    QTest::qWait(40);
    EXPECT_EQ(ended.size(), 0);
    if (scenario == 1)
      DetectionFixture::promotePayload(detector, successor.processId());
    else
      DetectionFixture::clear(detector);
  }
  ASSERT_TRUE(QTest::qWaitFor([&] {
    return engine.state() == RecordingEngine::State::Idle && !engine.hasDemand();
  }));
  ASSERT_EQ(ended.size(), 1);
  EXPECT_TRUE(ended.first()[1].toBool());
  EXPECT_FALSE(ended.first()[2].toBool()); // No automatic consumer continuation on permission closure.
  EXPECT_EQ(output->ends.load(), 1);
  EXPECT_EQ(errors.size(), 0);
  const bool offerSelection = scenario != 2 && scenario != 3;
  EXPECT_EQ(engine.captureResumeRequired(), offerSelection);
  EXPECT_EQ(engine.requiresCaptureSelection(), offerSelection);
  QTest::qWait(80);
  EXPECT_EQ(starts, 1);
  if (!offerSelection) return;
  EXPECT_EQ(engine.attach(std::make_shared<Output>(), name), 0u);
  EXPECT_EQ(settings.getRestoreTokenForIdentity(key, name), "existing-token");
  if (scenario == 4) {
    // User Stop dismisses the offer but must not clear the automatic-selection fence.
    RecordingEngineTestAccess::reconcile(engine);
    // Stop is an engine operation; there is no remaining output to detach.
    RecordingEngineTestAccess::dismissResume(engine);
    EXPECT_FALSE(engine.captureResumeRequired());
    EXPECT_TRUE(engine.requiresCaptureSelection());
    EXPECT_EQ(engine.attach(std::make_shared<Output>(), name), 0u);
    return;
  }
  if (scenario == 5) {
    DetectionFixture::publish(detector, successor.processId(), key);
    EXPECT_FALSE(engine.captureResumeRequired());
    EXPECT_FALSE(engine.requiresCaptureSelection()); // New instance, not an inherited fence.
    EXPECT_FALSE(engine.captureAuthorization()->selectionFences.contains(original));
    return;
  }
  if (scenario == 8) {
    DetectionFixture::addIndependentInstance(detector, runtime.processId(), "exe:/other", true);
    EXPECT_FALSE(engine.requiresCaptureSelection());
    DetectionFixture::selectInstance(detector, original);
    EXPECT_TRUE(engine.requiresCaptureSelection());
  }
  // Only a fresh, explicit selection may start a new generation, with no restore token.
  RecordingEngineTestAccess::source(engine, starts, destroyed, capture, {}, 0, false);
  const auto fresh = engine.attach(std::make_shared<Output>(), name, true);
  ASSERT_NE(fresh, 0u);
  ASSERT_TRUE(QTest::qWaitFor([&] { return starts == 2 && !capture.isNull(); }));
  EXPECT_TRUE(capture->config.windowOnly);
  EXPECT_TRUE(capture->config.waylandRestoreToken.isEmpty());
  if (scenario == 6 || scenario == 7) {
    capture->errorKind = scenario == 6 ? IVideoCaptureSource::CaptureErrorKind::Cancelled
                                      : IVideoCaptureSource::CaptureErrorKind::SourceNotReady;
    emit capture->captureError("explicit selection did not provide a stream");
    ASSERT_TRUE(QTest::qWaitFor([&] {
      return engine.state() == RecordingEngine::State::Idle && !engine.hasDemand();
    }));
    EXPECT_TRUE(engine.requiresCaptureSelection());
    QTest::qWait(80);
    EXPECT_EQ(starts, 2);
    if (scenario == 6) {
      EXPECT_EQ(errors.size(), 0);
    }
  } else {
    emit capture->ready();
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Running; }));
    EXPECT_FALSE(engine.requiresCaptureSelection());
    EXPECT_FALSE(engine.captureResumeRequired());
    engine.detach(fresh);
    ASSERT_TRUE(QTest::qWaitFor([&] { return engine.state() == RecordingEngine::State::Idle; }));
  }
}
INSTANTIATE_TEST_SUITE_P(PortalSelectionFences, EnginePortalFenceTest, ::testing::Range(0, 9),
    ([](const ::testing::TestParamInfo<int> &info) {
      return std::array{"LiveOwner", "VerifiedSuccessor", "NoPayload", "Manual", "DismissResume",
                        "Namesake", "CancelledPicker", "EmptyPicker", "SwitchSelection"}[info.param];
    }));
