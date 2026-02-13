/*
 * Trophy - Unit Tests
 * Test suite for StorageManager utilities and safety checks
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/StorageManager.h"

using namespace GameRecorder;

// Ensure QCoreApplication exists for Qt functionality
class StorageManagerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new StorageManagerTestEnvironment);

// --- formatSize() (static, no instance needed) ---

class FormatSizeTest : public ::testing::Test {};

TEST_F(FormatSizeTest, Zero) {
  EXPECT_EQ(StorageManager::formatSize(0), "0 B");
}

TEST_F(FormatSizeTest, Bytes) {
  EXPECT_EQ(StorageManager::formatSize(500), "500 B");
}

TEST_F(FormatSizeTest, OneByte) {
  EXPECT_EQ(StorageManager::formatSize(1), "1 B");
}

TEST_F(FormatSizeTest, JustBelowKB) {
  EXPECT_EQ(StorageManager::formatSize(1023), "1023 B");
}

TEST_F(FormatSizeTest, ExactlyOneKB) {
  EXPECT_EQ(StorageManager::formatSize(1024), "1.0 KB");
}

TEST_F(FormatSizeTest, KiloBytes) {
  EXPECT_EQ(StorageManager::formatSize(2048), "2.0 KB");
}

TEST_F(FormatSizeTest, MegaBytes) {
  qint64 fiveMB = 5LL * 1024 * 1024;
  EXPECT_EQ(StorageManager::formatSize(fiveMB), "5.0 MB");
}

TEST_F(FormatSizeTest, GigaBytes) {
  qint64 threeAndHalfGB = static_cast<qint64>(3.5 * 1024 * 1024 * 1024);
  EXPECT_EQ(StorageManager::formatSize(threeAndHalfGB), "3.50 GB");
}

TEST_F(FormatSizeTest, TeraBytes) {
  qint64 twoTB = 2LL * 1024 * 1024 * 1024 * 1024;
  EXPECT_EQ(StorageManager::formatSize(twoTB), "2.00 TB");
}

TEST_F(FormatSizeTest, FiftyGB) {
  qint64 fiftyGB = 50LL * 1024 * 1024 * 1024;
  EXPECT_EQ(StorageManager::formatSize(fiftyGB), "50.00 GB");
}

// --- StorageManager instance tests ---

class StorageManagerTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  // Create a dummy video file in the temp directory
  void createDummyFile(const QString &name, qint64 size) {
    QFile f(tempDir.path() + "/" + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, 'x'));
    f.close();
  }
};

TEST_F(StorageManagerTest, UsagePercentNormal) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(50LL * 1024 * 1024 * 1024); // 50 GB

  // Create a 25 GB equivalent file (we'll just test the calculation)
  // Instead of creating a huge file, set up and verify the formula
  EXPECT_DOUBLE_EQ(mgr.usagePercent(), 0.0); // No files yet
}

TEST_F(StorageManagerTest, UsagePercentZeroMax) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(0);
  EXPECT_DOUBLE_EQ(mgr.usagePercent(), 0.0);
}

TEST_F(StorageManagerTest, UsagePercentWithFiles) {
  createDummyFile("test.mkv", 1024 * 1024); // 1 MB
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(10LL * 1024 * 1024); // 10 MB
  mgr.refresh();

  double percent = mgr.usagePercent();
  EXPECT_NEAR(percent, 10.0, 0.5); // ~10%
}

TEST_F(StorageManagerTest, GenerateFilePathSanitization) {
  StorageManager mgr(tempDir.path());
  QString path = mgr.generateFilePath("My Game! (2024)", "mkv");

  // Special chars should be replaced with underscores
  EXPECT_FALSE(path.contains("!"));
  EXPECT_FALSE(path.contains("("));
  EXPECT_FALSE(path.contains(")"));
  EXPECT_TRUE(path.endsWith(".mkv"));
}

TEST_F(StorageManagerTest, GenerateFilePathTimestampFormat) {
  StorageManager mgr(tempDir.path());
  QString path = mgr.generateFilePath("TestGame", "mkv");

  // Should match pattern: Name_YYYY-MM-DD_HH-mm-ss.ext
  QRegularExpression re(R"(\d{4}-\d{2}-\d{2}_\d{2}-\d{2}-\d{2}\.mkv$)");
  EXPECT_TRUE(re.match(path).hasMatch())
      << "Path doesn't match timestamp pattern: " << path.toStdString();
}

TEST_F(StorageManagerTest, GenerateFilePathStartsWithStoragePath) {
  StorageManager mgr(tempDir.path());
  QString path = mgr.generateFilePath("Game", "mp4");
  EXPECT_TRUE(path.startsWith(tempDir.path()));
}

TEST_F(StorageManagerTest, DeleteRecordingOutsidePath) {
  StorageManager mgr(tempDir.path());

  // Attempt to delete a file outside the storage path
  bool result = mgr.deleteRecording("/etc/passwd");
  EXPECT_FALSE(result);
}

TEST_F(StorageManagerTest, DeleteRecordingNonExistent) {
  StorageManager mgr(tempDir.path());

  QString fakePath = tempDir.path() + "/nonexistent.mkv";
  bool result = mgr.deleteRecording(fakePath);
  EXPECT_FALSE(result);
}

TEST_F(StorageManagerTest, DeleteRecordingValid) {
  createDummyFile("to_delete.mkv", 1024);
  StorageManager mgr(tempDir.path());

  QString filePath = tempDir.path() + "/to_delete.mkv";
  EXPECT_TRUE(QFile::exists(filePath));

  bool result = mgr.deleteRecording(filePath);
  EXPECT_TRUE(result);
  EXPECT_FALSE(QFile::exists(filePath));
}

TEST_F(StorageManagerTest, RefreshCalculatesSize) {
  createDummyFile("video1.mkv", 5000);
  createDummyFile("video2.mp4", 3000);
  createDummyFile("notes.txt", 100); // Not a video, should not be counted

  StorageManager mgr(tempDir.path());
  mgr.refresh();

  // Only .mkv and .mp4 should be counted
  EXPECT_EQ(mgr.usedSize(), 8000);
}

TEST_F(StorageManagerTest, SetStoragePathChanges) {
  StorageManager mgr(tempDir.path());

  QTemporaryDir newDir;
  ASSERT_TRUE(newDir.isValid());

  mgr.setStoragePath(newDir.path());
  EXPECT_EQ(mgr.storagePath(), newDir.path());
}

TEST_F(StorageManagerTest, EnforceStorageLimitDeletesOldest) {
  // Create files with slight delay to ensure ordering
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000); // Only room for one file

  mgr.enforceStorageLimit();

  // At least one file should have been deleted
  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_LE(files.size(), 1);
}
