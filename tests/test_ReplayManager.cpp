/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/ReplayManager.h"
#include "core/RecordingManager.h"
#include "settings/SettingsManager.h"
#include "detection/GameDetectionService.h"
#include "DetectionFixture.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <gtest/gtest.h>

using namespace TrophyBackend;
namespace {
class PendingSource : public IVideoCaptureSource {
public:
  bool initialize(const CaptureConfig &) override { return true; }
  void setFrameCallback(FrameCallback) override {}
  bool startCapture() override { return true; }
  void stopCapture() override {}
  bool isCapturing() const override { return false; }
  CaptureCapabilities capabilities() const override { return {}; }
  QSize nativeResolution() const override { return {}; }
  QString name() const override { return QStringLiteral("fixture"); }
};
}
class ReplayManagerTest : public ::testing::Test {
protected:
  QTemporaryDir directory;
  QByteArray oldConfig;
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "replay-manager-tests";
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
TEST_F(ReplayManagerTest, InactiveByDefaultAndSaveFailsCleanly) {
  ReplayManager replay;
  QSignalSpy failed(&replay, &ReplayManager::replayFailed);
  EXPECT_FALSE(replay.isActive());
  EXPECT_FALSE(replay.isStarting());
  EXPECT_FALSE(replay.isRecoveringCapture());
  replay.saveReplay();
  ASSERT_EQ(failed.size(), 1);
  EXPECT_EQ(failed[0][0].toString(), QStringLiteral("Nothing buffered yet."));
}
TEST_F(ReplayManagerTest, DisabledReplayNeverRequestsProducerDemand) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(false);
  auto engine = std::make_shared<RecordingEngine>();
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  replay.startDvr("Fixture");
  QCoreApplication::processEvents();
  EXPECT_FALSE(engine->hasDemand());
  EXPECT_EQ(engine->state(), RecordingEngine::State::Idle);
}
TEST_F(ReplayManagerTest, PausePreventsNewDemand) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(true);
  auto engine = std::make_shared<RecordingEngine>();
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  replay.setPaused(true);
  replay.startDvr("Fixture");
  EXPECT_FALSE(engine->hasDemand());
  EXPECT_FALSE(replay.isStarting());
}
TEST_F(ReplayManagerTest, StopBeforeQueuedStartCancelsDemandWithoutCapture) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(true);
  settings.setReplayCachePath(directory.path());
  auto engine = std::make_shared<RecordingEngine>();
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  QSignalSpy failed(&replay, &ReplayManager::replayFailed);
  replay.startDvr("Fixture");
  ASSERT_TRUE(engine->hasDemand());
  replay.stopDvr(); // Before any queued producer reconciliation can run.
  ASSERT_TRUE(QTest::qWaitFor([&] { return !engine->hasDemand() && !replay.isStarting(); }));
  QCoreApplication::processEvents();
  EXPECT_EQ(engine->state(), RecordingEngine::State::Idle);
  EXPECT_EQ(failed.size(), 0);
}
TEST_F(ReplayManagerTest, EmptySaveDoesNotCancelPendingDemand) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(true);
  settings.setReplayCachePath(directory.path());
  auto engine = std::make_shared<RecordingEngine>();
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  QSignalSpy failed(&replay, &ReplayManager::replayFailed);
  replay.startDvr("Fixture");
  replay.saveReplay();
  EXPECT_EQ(failed.size(), 1);
  EXPECT_TRUE(engine->hasDemand());
  replay.stopDvr();
  ASSERT_TRUE(QTest::qWaitFor([&] { return !engine->hasDemand(); }));
}
TEST_F(ReplayManagerTest, StopCancelsDeferredDifferentGameStart) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(true);
  settings.setReplayCachePath(directory.path());
  auto engine = std::make_shared<RecordingEngine>();
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  replay.startDvr("First");
  replay.startDvr("Second");
  replay.stopDvr();
  ASSERT_TRUE(QTest::qWaitFor([&] { return !engine->hasDemand(); }));
  QTest::qWait(30);
  EXPECT_EQ(engine->state(), RecordingEngine::State::Idle);
  EXPECT_FALSE(replay.isStarting());
}
TEST_F(ReplayManagerTest, CaptureLifecycleSharesAuthorizationAcrossOutputModes) {
  SettingsManager settings;
  settings.setInstantReplayEnabled(true);
  GameDetectionService detector;
  detector.addToWhitelist("fixture", "Fixture");
  DetectionFixture::publish(detector, QCoreApplication::applicationPid(), "fixture");
  settings.setRestoreTokenForIdentity("fixture", "Fixture", "approved");
  auto engine = std::make_shared<RecordingEngine>();
  engine->setSettingsManager(&settings);
  engine->setGameDetectionService(&detector);
  RecordingManager recording(engine);
  ReplayManager replay(engine);
  replay.setSettingsManager(&settings);
  CaptureLifecycle lifecycle;
  lifecycle.configure(&detector, &settings);
  lifecycle.setAuthorization(engine->captureAuthorization());
  const auto identity = detector.captureIdentityInfo("Fixture");
  lifecycle.pin("Fixture", identity);
  ASSERT_TRUE(lifecycle.instanceIsCurrent());
  EXPECT_FALSE(recording.requiresCaptureSelection());
  lifecycle.fenceSelection(identity.value("detectedGameInstance").toString());
  EXPECT_TRUE(recording.requiresCaptureSelection());
  EXPECT_FALSE(lifecycle.canRecover(IVideoCaptureSource::CaptureErrorKind::SourceDestroyed, false));
  replay.startDvr("Fixture");
  EXPECT_FALSE(engine->hasDemand());
}
TEST_F(ReplayManagerTest, CaptureLifecycleSerializesRetriesAndCancelsLateDelivery) {
  SettingsManager settings;
  GameDetectionService detector;
  detector.addToWhitelist("fixture", "Fixture");
  DetectionFixture::publish(detector, QCoreApplication::applicationPid(), "fixture");
  settings.setRestoreTokenForIdentity("fixture", "Fixture", "approved");
  CaptureLifecycle lifecycle;
  lifecycle.configure(&detector, &settings);
  lifecycle.pin("Fixture", detector.captureIdentityInfo("Fixture"));
  using Kind = IVideoCaptureSource::CaptureErrorKind;
  EXPECT_TRUE(lifecycle.canRecover(Kind::SourceDestroyed, false));
  EXPECT_TRUE(lifecycle.canRecover(Kind::SourceNotReady, true));
  EXPECT_FALSE(lifecycle.canRecover(Kind::SourceNotReady, false));
  for (const auto kind : {Kind::Other, Kind::SourceUnavailable, Kind::SessionClosed})
    EXPECT_FALSE(lifecycle.canRecover(kind, true));
  int retries = 0, abandoned = 0;
  bool drained = false;
  const auto schedule = [&] {
    lifecycle.scheduleRetry(10, [&] { return drained; },
        [&] { ++retries; }, [&] { ++abandoned; });
  };
  schedule();
  schedule();
  QTest::qWait(40);
  EXPECT_EQ(retries, 0);
  drained = true;
  QTest::qWait(40);
  EXPECT_EQ(retries, 1);
  schedule();
  lifecycle.cancelRetry();
  QTest::qWait(40);
  EXPECT_EQ(retries, 1);
  schedule();
  settings.removeRestoreToken("fixture");
  QTest::qWait(40);
  EXPECT_EQ(retries, 1);
  EXPECT_EQ(abandoned, 1);
}
TEST_F(ReplayManagerTest, SharedSourceBindingRejectsRetiredEventsAndManualTokens) {
  SettingsManager settings;
  CaptureLifecycle lifecycle;
  lifecycle.configure(nullptr, &settings);
  PendingSource oldSource, replacement, manual;
  int generation = 1, ready = 0, errors = 0, stale = 0;
  const auto bind = [&](PendingSource &source, int attempt, bool isManual) {
    lifecycle.bindSource(&source, isManual ? "Manual Recording" : "Fixture",
        "fixture", isManual, [&, attempt] { return generation == attempt; },
        [&] { ++ready; }, [&](const QString &) { ++errors; },
        [&](const QString &) { ++stale; });
  };
  bind(oldSource, 1, false);
  emit oldSource.ready();
  emit oldSource.captureError("queued old source error");
  generation = 2;
  bind(replacement, 2, false);
  emit oldSource.restoreTokenUpdated("retired-token");
  emit oldSource.restoreTokenStale("retired-token");
  QCoreApplication::processEvents();
  EXPECT_EQ(ready, 0);
  EXPECT_EQ(errors, 0);
  EXPECT_EQ(stale, 0);
  EXPECT_TRUE(settings.getRestoreToken("fixture").isEmpty());
  emit replacement.restoreTokenUpdated("replacement-token");
  emit replacement.restoreTokenStale("replacement-token");
  emit replacement.ready();
  QCoreApplication::processEvents();
  EXPECT_EQ(ready, 1);
  EXPECT_EQ(stale, 1);
  EXPECT_EQ(settings.getRestoreToken("fixture"), "replacement-token");
  generation = 3;
  bind(manual, 3, true);
  emit manual.restoreTokenUpdated("manual-token");
  EXPECT_EQ(settings.getRestoreToken("fixture"), "replacement-token");
  EXPECT_TRUE(settings.getRestoreToken("Manual Recording").isEmpty());
}
