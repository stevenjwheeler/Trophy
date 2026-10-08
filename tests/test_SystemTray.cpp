/*
 * Trophy - Unit Tests
 * SystemTrayManager: menu/icon state and action signals without a tray
 * host (QApplication on the offscreen platform).
 */

#include <QApplication>
#include <QSignalSpy>
#include <gtest/gtest.h>

#include "core/SystemTrayManager.h"

using namespace TrophyBackend;

class SystemTrayTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QApplication::instance()) {
      qputenv("QT_QPA_PLATFORM", "offscreen");
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new SystemTrayTestEnvironment);

class SystemTrayTest : public ::testing::Test {};

TEST_F(SystemTrayTest, ConstructsWithoutTrayHost) {
  SystemTrayManager tray;
  // Offscreen has no tray host; construction and visibility requests
  // must be safe no-ops, never crashes.
  EXPECT_FALSE(tray.isAvailable());
  tray.setTrayVisible(true);
  tray.setTrayVisible(false);
}

TEST_F(SystemTrayTest, SetRecordingSwitchesMenuAndTooltip) {
  SystemTrayManager tray;
  EXPECT_EQ(tray.recordActionText(), "Start Recording");
  EXPECT_EQ(tray.toolTipText(), "Trophy");

  tray.setRecording(true, "Deadlock");
  EXPECT_EQ(tray.recordActionText(), "Stop Recording");
  EXPECT_EQ(tray.toolTipText(), "Trophy — Recording Deadlock");

  tray.setRecording(true); // no game name
  EXPECT_EQ(tray.toolTipText(), "Trophy — Recording");

  tray.setRecording(false);
  EXPECT_EQ(tray.recordActionText(), "Start Recording");
  EXPECT_EQ(tray.toolTipText(), "Trophy");
}

TEST_F(SystemTrayTest, ToggleWindowLabelFollowsVisibility) {
  SystemTrayManager tray;
  EXPECT_EQ(tray.toggleWindowActionText(), "Hide Trophy");

  tray.setWindowVisible(false);
  EXPECT_EQ(tray.toggleWindowActionText(), "Show Trophy");

  tray.setWindowVisible(true);
  EXPECT_EQ(tray.toggleWindowActionText(), "Hide Trophy");
}

TEST_F(SystemTrayTest, RecordActionEmitsMatchingSignal) {
  SystemTrayManager tray;
  QSignalSpy startSpy(&tray, &SystemTrayManager::startRecordingRequested);
  QSignalSpy stopSpy(&tray, &SystemTrayManager::stopRecordingRequested);

  tray.setRecording(false);
  tray.triggerRecordAction();
  EXPECT_EQ(startSpy.count(), 1);
  EXPECT_EQ(stopSpy.count(), 0);

  tray.setRecording(true, "Deadlock");
  tray.triggerRecordAction();
  EXPECT_EQ(startSpy.count(), 1);
  EXPECT_EQ(stopSpy.count(), 1);
}

TEST_F(SystemTrayTest, ToggleAndQuitActionsEmitSignals) {
  SystemTrayManager tray;
  QSignalSpy showSpy(&tray, &SystemTrayManager::showWindowRequested);
  QSignalSpy hideSpy(&tray, &SystemTrayManager::hideWindowRequested);
  QSignalSpy quitSpy(&tray, &SystemTrayManager::quitRequested);

  tray.setWindowVisible(true);
  tray.triggerToggleWindowAction();
  EXPECT_EQ(hideSpy.count(), 1);
  EXPECT_EQ(showSpy.count(), 0);

  tray.setWindowVisible(false);
  tray.triggerToggleWindowAction();
  EXPECT_EQ(showSpy.count(), 1);

  tray.triggerQuitAction();
  EXPECT_EQ(quitSpy.count(), 1);
}

TEST_F(SystemTrayTest, ProgressEnrichesRecordingTooltip) {
  SystemTrayManager tray;
  tray.setRecording(true, "Deadlock");

  // No counters yet: plain tooltip.
  EXPECT_EQ(tray.toolTipText(), "Trophy — Recording Deadlock");

  tray.setProgress(21000, 20132659); // 0:21, 19.2 MB
  EXPECT_EQ(tray.toolTipText(),
            "Trophy — Recording Deadlock • 0:21 • 19.2 MB");

  // Progress is ignored when idle and cleared when recording stops.
  tray.setRecording(false);
  EXPECT_EQ(tray.toolTipText(), "Trophy");
  tray.setProgress(60000, 99999999);
  EXPECT_EQ(tray.toolTipText(), "Trophy");
}

TEST_F(SystemTrayTest, ReplayActionEmitsSignal) {
  SystemTrayManager tray;
  QSignalSpy spy(&tray, &SystemTrayManager::saveReplayRequested);
  tray.triggerReplayAction();
  EXPECT_EQ(spy.count(), 1);
}
