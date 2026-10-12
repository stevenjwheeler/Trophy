/*
 * Trophy - Unit Tests
 * Test suite for RecordingManager storage enforcement behavior
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/RecordingManager.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"

using namespace TrophyBackend;

class RecordingManagerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new RecordingManagerTestEnvironment);

class RecordingManagerTest : public ::testing::Test {
protected:
  // NOTE: declared first so it outlives everything that reads config paths.
  QTemporaryDir configHome;
  QByteArray oldXdgConfigHome;
  QTemporaryDir tempDir;

  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    ASSERT_TRUE(configHome.isValid());
    // Isolate from the user's real ~/.config/trophy config: SettingsManager
    // saves on destruction, which must never touch the real config dir.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
  }

  void TearDown() override {
    if (oldXdgConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);
  }

  void createDummyFile(const QString &name, qint64 size) {
    QFile f(tempDir.path() + "/" + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, 'x'));
    f.close();
  }
};

TEST_F(RecordingManagerTest,
       StartRecordingAbortsWhenStorageLimitReachedAndAutoCleanupDisabled) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(false);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy stateSpy(&recorder, &RecordingManager::stateChanged);

  recorder.startRecording();

  EXPECT_FALSE(recorder.isRecording());
  EXPECT_EQ(startedSpy.count(), 0);
  ASSERT_GE(stateSpy.count(), 1);
  EXPECT_EQ(stateSpy.last()[0].toInt(),
            static_cast<int>(RecordingManager::RecordingState::Idle));
}

TEST_F(RecordingManagerTest,
       StartRecordingAbortsWhenAutoCleanupKeepsOnlyCurrentRecording) {
  createDummyFile("current.mkv", 7000);
  createDummyFile("old.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setExcludedRecordingPath(tempDir.path() + "/current.mkv");
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(true);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy stateSpy(&recorder, &RecordingManager::stateChanged);

  recorder.startRecording();

  EXPECT_FALSE(recorder.isRecording());
  EXPECT_EQ(startedSpy.count(), 0);
  ASSERT_GE(stateSpy.count(), 1);
  EXPECT_EQ(stateSpy.last()[0].toInt(),
            static_cast<int>(RecordingManager::RecordingState::Idle));
  EXPECT_GT(storage.usedSize(), storage.maxSize());
}

// --- Captured-source naming (manual recording while a game is detected
// must not inherit the detected game when the portal captured
// something else) ---

TEST_F(RecordingManagerTest, GenericSourceLabelsCarryNoIdentity) {
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel(""));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("Unknown Game"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("KWin Wayland"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("kwin_wayland"));
  EXPECT_TRUE(RecordingManager::isGenericSourceLabel("steam_app_1422450"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("Deadlock"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("deadlock.exe"));
  EXPECT_FALSE(RecordingManager::isGenericSourceLabel("Firefox"));
}

TEST_F(RecordingManagerTest, SourceLabelsAgreeAcrossTitleAndExeForms) {
  // Portal exe basenames agree with detector display names.
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("Deadlock", "deadlock.exe"));
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("deadlock.exe", "Deadlock"));
  EXPECT_TRUE(RecordingManager::sourceLabelsAgree("Firefox", "firefox"));
  // A different captured window disagrees with the detected game.
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Firefox", "Deadlock"));
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Deadlock", "Discord"));
  // Empty labels never agree (callers guard empties separately).
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("", "Deadlock"));
  EXPECT_FALSE(RecordingManager::sourceLabelsAgree("Deadlock", ""));
}

TEST_F(RecordingManagerTest,
       RepeatStartAttemptsWhileOverLimitNotifyEveryPress) {
  // Regression: record keybind / manual button while storage is full used
  // to abort silently after the first episode notice (latched signal, no
  // recordingError). Every explicit attempt must surface feedback.
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager storage(tempDir.path());
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(false);

  SettingsManager settings;
  RecordingManager recorder;
  recorder.setSettingsManager(&settings);
  recorder.setStorageManager(&storage);

  QSignalSpy startedSpy(&recorder, &RecordingManager::recordingStarted);
  QSignalSpy errorSpy(&recorder, &RecordingManager::recordingError);
  QSignalSpy cannotSpy(&storage,
                       &StorageManager::storageLimitCannotCleanup);

  for (int i = 0; i < 3; ++i) {
    recorder.startRecording();
    EXPECT_FALSE(recorder.isRecording());
  }

  EXPECT_EQ(startedSpy.count(), 0);
  // The in-app error dialog fires on every press, even with storage
  // notices toggled off; the notification signal fires on every press
  // too (forced, bypassing the episode latch).
  ASSERT_EQ(errorSpy.count(), 3);
  EXPECT_EQ(cannotSpy.count(), 3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(errorSpy.at(i).at(0).toString().contains("Storage full"));
  }
}
