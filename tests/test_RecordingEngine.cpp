/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/RecordingEngine.h"
#include "core/RecordingSession.h"
#include "capture/IVideoCaptureSource.h"
#include "encoding/IEncoder.h"
#include "settings/SettingsManager.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <gtest/gtest.h>

using namespace TrophyBackend;
namespace {
class Source final : public IVideoCaptureSource {
public:
  explicit Source(int &destroyed) : m_destroyed(destroyed) {}
  ~Source() override { ++m_destroyed; }
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback) override {}
  bool startCapture() override {
    QTimer::singleShot(0, this, [this] { emit ready(); });
    return true;
  }
  void stopCapture() override {}
  bool isCapturing() const override { return true; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {640, 480}; }
  QString name() const override { return QStringLiteral("Test window"); }
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
  bool begin(const EncodedStream &) override { ++begins; return true; }
  bool write(const EncodedFrame &packet) override { ++writes; last = packet; return !failWrite; }
  bool end() override { ++ends; return !failEnd; }
  bool started() const override { return writes > 0; }
  QString error() const override { return QStringLiteral("test output error"); }
  void setAudioHistory(quint64, const QJsonObject &) override {}
};
}
namespace TrophyBackend {
struct RecordingEngineTestAccess {
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
