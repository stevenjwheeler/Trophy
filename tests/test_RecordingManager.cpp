/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/RecordingManager.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"
#include "library/RecordingAnnotations.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace TrophyBackend {
// Manager policy only. Native capture and encoder tests belong to the engine;
// real file-output/packet regressions live in test_EncodedOutputs.cpp.
class RecordingManagerTest : public ::testing::Test {
protected:
  QTemporaryDir root;
  QByteArray previousConfig;
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "recording-policy-tests";
      static char *argv[] = {name, nullptr};
      new QCoreApplication(argc, argv);
    }
    ASSERT_TRUE(root.isValid());
    previousConfig = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", root.path().toUtf8());
  }
  void TearDown() override {
    if (previousConfig.isNull()) qunsetenv("XDG_CONFIG_HOME");
    else qputenv("XDG_CONFIG_HOME", previousConfig);
  }
  QString file(const QString &name, int bytes = 64) {
    const auto path = root.filePath(name);
    QFile output(path);
    EXPECT_TRUE(output.open(QIODevice::WriteOnly));
    EXPECT_EQ(output.write(QByteArray(bytes, 'x')), bytes);
    return path;
  }
  static quint64 id(const RecordingManager &manager) { return manager.m_id; }
  static void mediaPosition(RecordingManager &manager, qint64 position) {
    manager.m_output->m_position.store(position);
  }
  static void completedPart(RecordingManager &manager, const QString &path,
                            bool succeeded, bool quiet) {
    manager.m_output->m_parts.append({1, path, 1000, 64, succeeded, {}});
    emit manager.m_engine->generationEnded(manager.m_id, succeeded, quiet,
        quiet ? RecordingEngine::EndReason::Handoff : RecordingEngine::EndReason::Stopped);
  }
};

TEST_F(RecordingManagerTest, StorageFullRejectsEveryPressWithoutPublishingAnOutput) {
  file("first.mkv", 5000);
  file("second.mkv", 5000);
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(false);
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy errors(&manager, &RecordingManager::recordingError);
  QSignalSpy prepared(&manager, &RecordingManager::recordingPathPrepared);
  QSignalSpy cannotCleanup(&storage, &StorageManager::storageLimitCannotCleanup);
  for (int i = 0; i < 3; ++i) manager.startRecording("Fixture");
  EXPECT_EQ(errors.size(), 3);
  EXPECT_EQ(cannotCleanup.size(), 3);
  EXPECT_TRUE(prepared.isEmpty());
  EXPECT_FALSE(engine->hasDemand());
  EXPECT_EQ(manager.state(), RecordingManager::RecordingState::Idle);
}

TEST_F(RecordingManagerTest, AutoCleanupDoesNotDeleteAnExcludedRecording) {
  const auto current = file("current.mkv", 7000);
  file("old.mkv", 5000);
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  storage.setExcludedRecordingPath(current);
  storage.setMaxSize(6000);
  storage.setAutoDeleteOldest(true);
  RecordingManager manager;
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy errors(&manager, &RecordingManager::recordingError);
  manager.startRecording("Fixture");
  EXPECT_EQ(errors.size(), 1);
  EXPECT_TRUE(QFileInfo::exists(current));
  EXPECT_EQ(manager.state(), RecordingManager::RecordingState::Idle);
}

TEST_F(RecordingManagerTest, RepeatedStartKeepsOneDemandAndOneReservedPath) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy prepared(&manager, &RecordingManager::recordingPathPrepared);
  manager.startRecording("Fixture");
  const auto first = id(manager);
  const auto path = manager.currentFilePath();
  manager.startRecording("Different game");
  EXPECT_NE(first, 0u);
  EXPECT_EQ(id(manager), first);
  EXPECT_EQ(manager.currentFilePath(), path);
  EXPECT_EQ(prepared.size(), 1);
  EXPECT_TRUE(engine->hasDemand());
  EXPECT_FALSE(QFileInfo::exists(path));
  manager.stopRecording(); // Cancel before queued producer startup.
  EXPECT_FALSE(engine->hasDemand());
}

TEST_F(RecordingManagerTest, PreparedOutputUsesSafeMkvPathBeforeAnyCaptureOrFileCreation) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy prepared(&manager, &RecordingManager::recordingPathPrepared);
  manager.startRecording("../../Outside:Title\\name.mp4");
  ASSERT_EQ(prepared.size(), 1);
  const auto path = manager.currentFilePath();
  EXPECT_EQ(prepared.first()[0].toString(), path);
  EXPECT_EQ(QFileInfo(path).absolutePath(), root.path());
  EXPECT_TRUE(QRegularExpression(
      "^Outside_Title_name_mp4_[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}\\.mkv$")
      .match(QFileInfo(path).fileName()).hasMatch());
  EXPECT_FALSE(QFileInfo::exists(path));
  EXPECT_EQ(engine->state(), RecordingEngine::State::Idle); // Queued startup has not run.
  manager.stopRecording(); // Retire demand before any real capture can start.
  EXPECT_FALSE(engine->hasDemand());
}

TEST_F(RecordingManagerTest, BookmarksRequireActiveVideoAndSurviveStop) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  EXPECT_FALSE(manager.addBookmark());
  manager.startRecording("Fixture");
  EXPECT_FALSE(manager.addBookmark());
  emit engine->outputStarted(id(manager));
  EXPECT_FALSE(manager.addBookmark()); // Started, but no media origin yet.
  const auto path = manager.currentFilePath();
  QFile output(path);
  ASSERT_TRUE(output.open(QIODevice::WriteOnly));
  output.write(QByteArray(64, 'x'));
  output.close();
  mediaPosition(manager, 1234);
  ASSERT_TRUE(manager.addBookmark());
  manager.stopRecording();
  EXPECT_FALSE(manager.addBookmark());
  const auto marks = RecordingAnnotations::bookmarks(path);
  ASSERT_EQ(marks.size(), 1);
  EXPECT_EQ(marks[0].toMap()["positionMs"].toLongLong(), 1234);
}

TEST_F(RecordingManagerTest, SuccessfulCompletionPublishesOnlyAfterOutputSettles) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy stopped(&manager, &RecordingManager::recordingStopped);
  QSignalSpy complete(&manager, &RecordingManager::recordingCompletionReady);
  QSignalSpy finished(&manager, &RecordingManager::recordingFinished);
  manager.startRecording("Fixture");
  const auto output = id(manager);
  const auto path = manager.currentFilePath();
  completedPart(manager, path, true, false);
  EXPECT_EQ(stopped.size(), 1);
  EXPECT_TRUE(complete.isEmpty());
  emit engine->outputStopped(output, true, {});
  ASSERT_EQ(complete.size(), 1);
  EXPECT_EQ(complete[0][0].toString(), path);
  EXPECT_EQ(finished.size(), 1);
  EXPECT_EQ(manager.state(), RecordingManager::RecordingState::Idle);
  EXPECT_TRUE(manager.currentFilePath().isEmpty());
  emit engine->outputStopped(output, true, {});
  EXPECT_EQ(complete.size(), 1);
  EXPECT_EQ(finished.size(), 1);
}

TEST_F(RecordingManagerTest, FailedOutputPreservesFootageAndNeverAnnouncesSaved) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy errors(&manager, &RecordingManager::recordingError);
  QSignalSpy complete(&manager, &RecordingManager::recordingCompletionReady);
  manager.startRecording("Fixture");
  const auto output = id(manager);
  const auto path = manager.currentFilePath();
  QFile footage(path);
  ASSERT_TRUE(footage.open(QIODevice::WriteOnly));
  footage.write("preserved footage");
  footage.close();
  emit engine->outputError(output, "write failed");
  completedPart(manager, path, false, false);
  emit engine->outputStopped(output, false, "write failed");
  EXPECT_EQ(errors.size(), 1);
  EXPECT_TRUE(complete.isEmpty());
  EXPECT_TRUE(manager.lastRecordingFailed());
  EXPECT_TRUE(QFileInfo::exists(path));
}

TEST_F(RecordingManagerTest, HandoffReservesNextPartAndDefersCompletion) {
  SettingsManager settings;
  settings.setRecordingsPath(root.path());
  StorageManager storage(root.path());
  auto engine = std::make_shared<RecordingEngine>();
  RecordingManager manager(engine);
  manager.setSettingsManager(&settings);
  manager.setStorageManager(&storage);
  QSignalSpy prepared(&manager, &RecordingManager::recordingPathPrepared);
  QSignalSpy stopped(&manager, &RecordingManager::recordingStopped);
  QSignalSpy complete(&manager, &RecordingManager::recordingCompletionReady);
  manager.startRecording("Fixture");
  const auto path = manager.currentFilePath();
  completedPart(manager, path, true, true);
  EXPECT_EQ(stopped.size(), 1);
  EXPECT_TRUE(stopped[0][3].toBool());
  EXPECT_EQ(prepared.size(), 2);
  EXPECT_TRUE(complete.isEmpty());
  EXPECT_EQ(manager.state(), RecordingManager::RecordingState::Initializing);
  emit engine->outputStopped(id(manager), true, {});
  ASSERT_EQ(complete.size(), 1);
  EXPECT_EQ(complete[0][0].toString(), path);
}
} // namespace TrophyBackend
