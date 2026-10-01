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
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

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
