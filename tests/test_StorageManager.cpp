/*
 * Trophy - Unit Tests
 * Test suite for StorageManager utilities and safety checks
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/StorageManager.h"
#include "library/RecordingAnnotations.h"
#include <QSignalSpy>

using namespace TrophyBackend;

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

  void observeRefreshes(StorageManager &mgr, int &count) {
    mgr.m_refreshObserverForTest = [&count]() { ++count; };
  }

  void deliverDirectoryBurst(StorageManager &mgr) {
    // Exercise the real watcher connection without relying on filesystem
    // event delivery or entering the event loop.
    for (int i = 0; i < 50; ++i) {
      ASSERT_TRUE(QMetaObject::invokeMethod(
          &mgr.m_watcher, "directoryChanged", Qt::DirectConnection,
          Q_ARG(QString, tempDir.path())));
    }
  }

  bool debounceActive(const StorageManager &mgr) {
    return mgr.m_debounceTimer.isActive();
  }

  void expireDebounce(StorageManager &mgr) {
    ASSERT_TRUE(mgr.m_debounceTimer.isActive());
    ASSERT_TRUE(mgr.m_debounceTimer.isSingleShot());
    EXPECT_EQ(mgr.m_debounceTimer.interval(), 500);
    // Qt stops a single-shot before emitting timeout. Reproduce that
    // ordering and invoke the existing production timeout connection.
    mgr.m_debounceTimer.stop();
    ASSERT_TRUE(QMetaObject::invokeMethod(
        &mgr.m_debounceTimer, "timeout", Qt::DirectConnection));
  }

  void beginBulkSuppression(StorageManager &mgr) {
    // Model an in-flight worker without racing an actual background unlink.
    ++mgr.m_bulkDeletesActive;
  }

  void finishBulkSuppression(StorageManager &mgr) {
    mgr.finishBulkDelete({}, 0);
  }

  // Create a dummy video file in the temp directory
  void createDummyFile(const QString &name, qint64 size) {
    QFile f(tempDir.path() + "/" + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, 'x'));
    f.close();
  }

  // Create a sparse file with the given apparent size (instant, ~no disk
  // use) and modification time. Lets cleanup tests work at GB scale.
  void createSparseFile(const QString &name, qint64 size,
                        const QDateTime &mtime) {
    QFile f(tempDir.path() + "/" + name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_TRUE(f.resize(size));
    // Instance setFileTime operates on the open handle: pin the mtime
    // before closing (a closed instance reports false).
    ASSERT_TRUE(f.setFileTime(mtime, QFileDevice::FileModificationTime));
    f.close();
    ASSERT_EQ(f.size(), size);
  }
};

TEST_F(StorageManagerTest, UsagePercentNormal) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(50LL * 1024 * 1024 * 1024); // 50 GB

  // Create a 25 GB equivalent file (we'll just test the calculation)
  // Instead of creating a huge file, set up and verify the formula
  EXPECT_DOUBLE_EQ(mgr.usagePercent(), 0.0); // No files yet
}

TEST_F(StorageManagerTest, CleanupKeepsProtectedAndReportsWhenNothingEligible) {
  createDummyFile("Keep.mkv", 8192);
  createDummyFile("Delete.mkv", 8192);
  const QString keep = tempDir.filePath("Keep.mkv");
  ASSERT_TRUE(RecordingAnnotations::setProtected(keep, true));
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(4096);
  mgr.setAutoDeleteOldest(true);
  QSignalSpy blocked(&mgr, &StorageManager::storageLimitCannotCleanup);
  EXPECT_FALSE(mgr.enforceStorageLimit(true, true));
  EXPECT_TRUE(QFile::exists(keep));
  EXPECT_FALSE(QFile::exists(tempDir.filePath("Delete.mkv")));
  EXPECT_GE(blocked.count(), 1);
  // Protection is not a ban on explicit user deletion.
  EXPECT_TRUE(mgr.deleteRecording(keep));
  EXPECT_FALSE(QFile::exists(RecordingAnnotations::path(keep)));
}

TEST_F(StorageManagerTest, RenamePreservesProtectionAndBookmarks) {
  createDummyFile("Game.mkv", 8192);
  const QString from = tempDir.filePath("Game.mkv");
  ASSERT_TRUE(RecordingAnnotations::setProtected(from, true));
  ASSERT_TRUE(RecordingAnnotations::addBookmark(from, 12345));
  StorageManager mgr(tempDir.path());
  const QString to = mgr.renameRecording(from, "Renamed");
  ASSERT_FALSE(to.isEmpty());
  EXPECT_TRUE(RecordingAnnotations::isProtected(to));
  ASSERT_EQ(RecordingAnnotations::bookmarks(to).size(), 1);
  EXPECT_EQ(RecordingAnnotations::bookmarks(to)[0].toMap()["positionMs"].toLongLong(), 12345);
  EXPECT_FALSE(QFile::exists(RecordingAnnotations::path(from)));
}

TEST_F(StorageManagerTest, RenameRefusesExistingAnnotationsWithoutLosingProtection) {
  createDummyFile("Game.mkv", 8192);
  const QString from = tempDir.filePath("Game.mkv");
  ASSERT_TRUE(RecordingAnnotations::setProtected(from, true));
  QFile collision(RecordingAnnotations::path(tempDir.filePath("Renamed.mkv")));
  ASSERT_TRUE(collision.open(QIODevice::WriteOnly));
  collision.write("{\"protected\":false}");
  collision.close();
  StorageManager mgr(tempDir.path());
  EXPECT_TRUE(mgr.renameRecording(from, "Renamed").isEmpty());
  EXPECT_TRUE(QFile::exists(from));
  EXPECT_TRUE(RecordingAnnotations::isProtected(from));
}

TEST_F(StorageManagerTest, CorruptAnnotationsFailClosedAndAreNotOverwritten) {
  createDummyFile("Game.mkv", 8192);
  const QString video = tempDir.filePath("Game.mkv");
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".trophy"));
  QFile file(RecordingAnnotations::path(video));
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("{broken");
  file.close();
  EXPECT_TRUE(RecordingAnnotations::isProtected(video));
  EXPECT_FALSE(RecordingAnnotations::setProtected(video, false));
  EXPECT_FALSE(RecordingAnnotations::addBookmark(video, 1));
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_EQ(file.readAll(), "{broken");
  file.close();
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(1);
  EXPECT_FALSE(mgr.enforceStorageLimit(true));
  EXPECT_TRUE(QFile::exists(video));
}

TEST_F(StorageManagerTest, AnnotationWritesRejectSymlinkDirectoryAndInvalidTimes) {
  createDummyFile("Game.mkv", 8192);
  const QString video = tempDir.filePath("Game.mkv");
  QTemporaryDir outside;
  ASSERT_TRUE(outside.isValid());
  ASSERT_TRUE(QFile::link(outside.path(), tempDir.filePath(".trophy")));
  EXPECT_TRUE(RecordingAnnotations::isProtected(video));
  EXPECT_FALSE(RecordingAnnotations::setProtected(video, true));
  EXPECT_FALSE(RecordingAnnotations::addBookmark(video, 1234));
  EXPECT_TRUE(QDir(outside.path()).entryList(QDir::Files).isEmpty());
  ASSERT_TRUE(QFile::remove(tempDir.filePath(".trophy")));
  EXPECT_FALSE(RecordingAnnotations::addBookmark(video, -1));
  EXPECT_FALSE(RecordingAnnotations::addBookmark(video, 86400001));
  ASSERT_TRUE(RecordingAnnotations::setProtected(video, true));
  ASSERT_TRUE(RecordingAnnotations::addBookmark(video, 1234));
  EXPECT_TRUE(RecordingAnnotations::isProtected(video));
}

TEST_F(StorageManagerTest, SpecialAnnotationFileFailsClosedWithoutOpeningIt) {
  createDummyFile("Game.mkv", 8192);
  const QString video = tempDir.filePath("Game.mkv");
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".trophy"));
  ASSERT_EQ(::mkfifo(QFile::encodeName(RecordingAnnotations::path(video)).constData(), 0600), 0);
  EXPECT_TRUE(RecordingAnnotations::isProtected(video));
  EXPECT_FALSE(RecordingAnnotations::addBookmark(video, 123));
  StorageManager mgr(tempDir.path());
  EXPECT_TRUE(mgr.renameRecording(video, "Renamed").isEmpty());
  EXPECT_TRUE(QFile::exists(video));
}

TEST_F(StorageManagerTest, UsagePercentZeroMax) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(0);
  EXPECT_DOUBLE_EQ(mgr.usagePercent(), 0.0);
}

TEST_F(StorageManagerTest, EnforceStorageLimitNoLimitDoesNotDelete) {
  createDummyFile("unlimited.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(0);

  mgr.enforceStorageLimit();

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 1);
}

TEST_F(StorageManagerTest, UsagePercentWithFiles) {
  createDummyFile("test.mkv", 1024 * 1024); // 1 MB
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(10LL * 1024 * 1024); // 10 MB
  mgr.refresh();

  double percent = mgr.usagePercent();
  EXPECT_NEAR(percent, 10.0, 0.5); // ~10%
}

TEST_F(StorageManagerTest, DeleteRecordingOutsidePath) {
  // Path-traversal guard: the victim is a sacrificial file in a sibling
  // temp dir OUTSIDE the storage path — never a system path. If this guard
  // ever regresses, the worst case is a temp file disappearing.
  // Make the sibling path share the full storage prefix; startsWith()
  // alone would mistakenly accept it as inside the library.
  QTemporaryDir outsideDir(tempDir.path() + "-outside-XXXXXX");
  ASSERT_TRUE(outsideDir.isValid());
  const QString outsidePath = outsideDir.path() + "/outside.mkv";
  QFile f(outsidePath);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(1024, 'x'));
  f.close();

  StorageManager mgr(tempDir.path());
  EXPECT_FALSE(mgr.deleteRecording(outsidePath));
  EXPECT_TRUE(QFile::exists(outsidePath)); // untouched
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

TEST_F(StorageManagerTest, DirectoryEventBurstExpiryRefreshesOnce) {
  StorageManager mgr(tempDir.path());
  int refreshes = 0;
  observeRefreshes(mgr, refreshes);
  createDummyFile("imported.mkv", 4096);
  QSignalSpy changed(&mgr, &StorageManager::usedSizeChanged);

  deliverDirectoryBurst(mgr);
  EXPECT_TRUE(debounceActive(mgr));
  EXPECT_EQ(refreshes, 0);
  EXPECT_EQ(mgr.usedSize(), 0);
  EXPECT_EQ(changed.count(), 0);

  expireDebounce(mgr);
  EXPECT_FALSE(debounceActive(mgr));
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(mgr.usedSize(), 4096);
  EXPECT_EQ(changed.count(), 1);

  // A second burst with unchanged size still has exactly one real scan;
  // usedSizeChanged alone would conceal accidental repeated refreshes.
  deliverDirectoryBurst(mgr);
  EXPECT_EQ(refreshes, 1);
  expireDebounce(mgr);
  EXPECT_EQ(refreshes, 2);
  EXPECT_EQ(changed.count(), 1);
}

TEST_F(StorageManagerTest, DirectoryEventBurstExpiryCleansUpInOnePass) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(1024);
  mgr.setAutoDeleteOldest(true);
  int refreshes = 0;
  observeRefreshes(mgr, refreshes);
  // Keep a survivor so final usage differs from the initially empty library.
  createDummyFile("keeper.mkv", 512);
  mgr.setExcludedRecordingPath(tempDir.path() + "/keeper.mkv");
  for (int i = 0; i < 10; ++i) {
    createDummyFile(QString("imported_%1.mkv").arg(i), 4096);
  }
  QSignalSpy changed(&mgr, &StorageManager::usedSizeChanged);
  QSignalSpy deleted(&mgr, &StorageManager::recordingDeleted);

  deliverDirectoryBurst(mgr);

  EXPECT_TRUE(debounceActive(mgr));
  EXPECT_EQ(refreshes, 0);
  EXPECT_EQ(mgr.usedSize(), 0);
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(deleted.count(), 0);

  expireDebounce(mgr);
  EXPECT_FALSE(debounceActive(mgr));
  // One entry scan and one settle scan, not one refresh per deletion.
  EXPECT_EQ(refreshes, 2);
  EXPECT_EQ(changed.count(), 2);
  EXPECT_EQ(deleted.count(), 10);
  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(QFile::exists(
        tempDir.path() + QString("/imported_%1.mkv").arg(i)));
  }
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/keeper.mkv"));
  EXPECT_EQ(mgr.usedSize(), 512);
}

TEST_F(StorageManagerTest, BulkDeleteSuppressesWatcherBurstAndPendingExpiry) {
  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(1024);
  mgr.setAutoDeleteOldest(true);
  int refreshes = 0;
  observeRefreshes(mgr, refreshes);
  createDummyFile("unselected.mkv", 4096);
  QSignalSpy deleted(&mgr, &StorageManager::recordingDeleted);
  QSignalSpy finished(&mgr, &StorageManager::bulkDeleteFinished);

  beginBulkSuppression(mgr);
  deliverDirectoryBurst(mgr);
  EXPECT_FALSE(debounceActive(mgr));
  EXPECT_EQ(refreshes, 0);
  finishBulkSuppression(mgr);
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(finished.count(), 1);
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/unselected.mkv"));
  EXPECT_EQ(deleted.count(), 0);

  // A timer armed before the bulk delete must also honor suppression.
  deliverDirectoryBurst(mgr);
  ASSERT_TRUE(debounceActive(mgr));
  beginBulkSuppression(mgr);
  deliverDirectoryBurst(mgr);
  expireDebounce(mgr);
  EXPECT_FALSE(debounceActive(mgr));
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(deleted.count(), 0);
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/unselected.mkv"));
  finishBulkSuppression(mgr);
  EXPECT_EQ(refreshes, 2);
  EXPECT_EQ(finished.count(), 2);
  EXPECT_EQ(mgr.usedSize(), 4096);

  // Completion restores normal watcher enforcement, not permanent suppression.
  deliverDirectoryBurst(mgr);
  expireDebounce(mgr);
  EXPECT_EQ(refreshes, 4);
  EXPECT_EQ(deleted.count(), 1);
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/unselected.mkv"));
  EXPECT_EQ(mgr.usedSize(), 0);
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
  mgr.setAutoDeleteOldest(true);

  mgr.enforceStorageLimit();

  // At least one file should have been deleted
  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_LE(files.size(), 1);
}

TEST_F(StorageManagerTest, EnforceStorageLimitShrinksDownUntilUnderNewLimit) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("middle.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.refresh();
  mgr.setMaxSize(9000); // Should force removal of at least two files
  mgr.setAutoDeleteOldest(true);
  mgr.enforceStorageLimit();

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_LE(files.size(), 1);
  EXPECT_LE(mgr.usedSize(), 9000);
}

TEST_F(StorageManagerTest, EnforceStorageLimitSkipsExcludedRecording) {
  createDummyFile("current.mkv", 5000);
  createDummyFile("old.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setExcludedRecordingPath(tempDir.path() + "/current.mkv");
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);

  mgr.enforceStorageLimit();

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 1);
  EXPECT_TRUE(files.contains("current.mkv"));
  EXPECT_LE(mgr.usedSize(), 6000);
}

TEST_F(StorageManagerTest, CleanupFindsNestedMediaCountedByUsage) {
  const QString nestedDir = tempDir.path() + "/archive/old";
  ASSERT_TRUE(QDir().mkpath(nestedDir));
  QFile nested(nestedDir + "/nested.mkv");
  ASSERT_TRUE(nested.open(QIODevice::WriteOnly));
  ASSERT_EQ(nested.write(QByteArray(8192, 'x')), 8192);
  nested.close();

  StorageManager mgr(tempDir.path());
  EXPECT_EQ(mgr.usedSize(), 8192);
  mgr.setMaxSize(4096);
  mgr.setAutoDeleteOldest(true);
  EXPECT_TRUE(mgr.enforceStorageLimit());
  EXPECT_FALSE(QFile::exists(nestedDir + "/nested.mkv"));
  EXPECT_EQ(mgr.usedSize(), 0);
}

TEST_F(StorageManagerTest,
       EnforceStorageLimitDoesNotDeleteWhenAutoCleanupDisabled) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(false);

  mgr.enforceStorageLimit();

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 2);
  EXPECT_GT(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest, SetMaxSizeDoesNotDeleteImmediately) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setAutoDeleteOldest(true);
  mgr.setMaxSize(6000);

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 2);
  EXPECT_GT(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest,
       EnableAutoDeleteDoesNotDeleteImmediatelyWhenOverLimit) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 2);
  EXPECT_GT(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest, RequestCleanupDeletesOldFilesWhenConfirmed) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);

  EXPECT_TRUE(mgr.requestCleanup());

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_LE(files.size(), 1);
  EXPECT_LE(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest,
       RequestCleanupForceDeletesOldFilesWhenAutoCleanupDisabled) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(false);

  EXPECT_TRUE(mgr.requestCleanup(true));

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_LE(files.size(), 1);
  EXPECT_LE(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest, ExcludedOldestDoesNotBlockCleanupOfNewer) {
  // Regression: auto-cleanup used to give up entirely when the excluded
  // (in-progress) recording sorted oldest, leaving storage full.
  createDummyFile("keeper.mkv", 5000); // will be excluded AND oldest
  createDummyFile("victim.mkv", 5000);
  // Pin mtimes explicitly: same-millisecond creation ties made the old
  // sort order ambiguous, which is exactly what hid this bug.
  auto pinMtime = [](const QString &path, const QDateTime &dt) {
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadWrite));
    EXPECT_TRUE(f.setFileTime(dt, QFileDevice::FileModificationTime));
    f.close();
  };
  pinMtime(tempDir.path() + "/keeper.mkv",
           QDateTime(QDate(2020, 1, 1), QTime(0, 0)));
  pinMtime(tempDir.path() + "/victim.mkv",
           QDateTime(QDate(2021, 1, 1), QTime(0, 0)));

  StorageManager mgr(tempDir.path());
  mgr.setExcludedRecordingPath(tempDir.path() + "/keeper.mkv");
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);

  EXPECT_TRUE(mgr.enforceStorageLimit());
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/keeper.mkv"));
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/victim.mkv"));
  EXPECT_LE(mgr.usedSize(), 6000);
}

TEST_F(StorageManagerTest,
       EnforceStorageLimitFailsWhenOnlyExcludedRecordingRemains) {
  createDummyFile("current.mkv", 7000);
  createDummyFile("old.mkv", 5000);

  StorageManager mgr(tempDir.path());
  mgr.setExcludedRecordingPath(tempDir.path() + "/current.mkv");
  QSignalSpy spy(&mgr, &StorageManager::storageLimitCannotCleanup);
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);

  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_EQ(spy.count(), 1);

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 1);
  EXPECT_TRUE(files.contains("current.mkv"));
  EXPECT_GT(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest, EnforceStorageLimitStopsDeletingOnceUnderLimit) {
  // Regression for the 2026-10-13 incident: with ~100 GB used against a
  // ~100 GB limit and a recording in progress, auto-cleanup deleted the
  // ENTIRE library (20 files) instead of just enough. The pass's running
  // total never shrank, so the loop consumed the whole snapshot and only
  // stopped at the excluded in-progress file. Every "Storage limit
  // exceeded" line in the log printed the same frozen size.
  constexpr qint64 kFileSize = 5LL * 1024 * 1024 * 1024; // 5 GiB, sparse
  const QDateTime base(QDate(2026, 10, 1), QTime(0, 0));
  for (int i = 0; i < 20; ++i) {
    const QString name =
        QStringLiteral("rec_%1.mkv").arg(i, 3, 10, QChar('0'));
    createSparseFile(name, kFileSize, base.addSecs(i * 3600));
  }
  createSparseFile("current.mkv", kFileSize, base.addSecs(21 * 3600));

  StorageManager mgr(tempDir.path());
  mgr.setExcludedRecordingPath(tempDir.path() + "/current.mkv");
  ASSERT_EQ(mgr.usedSize(), 21 * kFileSize);
  // Room for 18 files: exactly the 3 oldest must go (2 would leave
  // 19 files = 95 GiB > 93 GiB).
  mgr.setMaxSize(21 * kFileSize - 12 * 1024 * 1024 * 1024LL);
  mgr.setAutoDeleteOldest(true);

  EXPECT_TRUE(mgr.enforceStorageLimit());

  // Exactly the 3 oldest are gone; everything else (incl. excluded) stays.
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/rec_000.mkv"));
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/rec_001.mkv"));
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/rec_002.mkv"));
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/rec_003.mkv"));
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/rec_019.mkv"));
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/current.mkv"));
  EXPECT_EQ(mgr.usedSize(), 18 * kFileSize);
  EXPECT_LE(mgr.usedSize(), mgr.maxSize());

  // A follow-up pass (the 1 Hz progress ticker) must be a no-op.
  const int filesBefore =
      QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files).size();
  EXPECT_TRUE(mgr.enforceStorageLimit());
  EXPECT_EQ(QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files).size(),
            filesBefore);
}

TEST_F(StorageManagerTest,
       EnforceStorageLimitDoesNotDeleteOverLimitWhenAutoCleanupDisabled) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);

  StorageManager mgr(tempDir.path());
  QSignalSpy spy(&mgr, &StorageManager::storageLimitCannotCleanup);
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(false);

  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_EQ(spy.count(), 1);

  QDir dir(tempDir.path());
  auto files = dir.entryList({"*.mkv"}, QDir::Files);
  EXPECT_EQ(files.size(), 2);
  EXPECT_GT(mgr.usedSize(), mgr.maxSize());
}

TEST_F(StorageManagerTest, NoWarningNearButUnderLimit) {
  // 95% full with auto-cleanup on: the old >90% early warning fired
  // storageLimitReached on every enforce call without deleting anything.
  createDummyFile("a.mkv", 9500);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(10000);
  mgr.setAutoDeleteOldest(true);

  QSignalSpy reached(&mgr, &StorageManager::storageLimitReached);
  QSignalSpy cannot(&mgr, &StorageManager::storageLimitCannotCleanup);
  EXPECT_TRUE(mgr.enforceStorageLimit());
  EXPECT_TRUE(mgr.enforceStorageLimit());
  EXPECT_EQ(reached.count(), 0);
  EXPECT_EQ(cannot.count(), 0);

  QDir dir(tempDir.path());
  EXPECT_EQ(dir.entryList({"*.mkv"}, QDir::Files).size(), 1);
}

TEST_F(StorageManagerTest, OverLimitNoticeSentOncePerEpisode) {
  createDummyFile("a.mkv", 4000);
  createDummyFile("b.mkv", 4000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(10000);

  QSignalSpy reached(&mgr, &StorageManager::storageLimitReached);
  QSignalSpy cannot(&mgr, &StorageManager::storageLimitCannotCleanup);
  createDummyFile("c.mkv", 4000); // 12000 > 10000, cleanup off
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_EQ(cannot.count(), 1);
  EXPECT_EQ(reached.count(), 0);

  // Back under the limit resets the latch: the next episode notifies again.
  ASSERT_TRUE(mgr.deleteRecording(tempDir.path() + "/a.mkv"));
  ASSERT_TRUE(mgr.deleteRecording(tempDir.path() + "/b.mkv"));
  EXPECT_TRUE(mgr.enforceStorageLimit());
  createDummyFile("d.mkv", 8000);
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_EQ(cannot.count(), 2);
}

TEST_F(StorageManagerTest, ForcedNotifyRepeatsEveryExplicitAttempt) {
  // Explicit user-initiated start attempts (record keybind / manual button
  // while over limit) must produce feedback on EVERY press, not just the
  // first of the episode. Background ticks stay on the once-per-episode
  // latch; forceNotify=true bypasses it.
  createDummyFile("a.mkv", 4000);
  createDummyFile("b.mkv", 4000);

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(10000);
  mgr.setAutoDeleteOldest(false);

  QSignalSpy cannot(&mgr, &StorageManager::storageLimitCannotCleanup);
  createDummyFile("c.mkv", 4000); // 12000 > 10000, cleanup off
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_FALSE(mgr.enforceStorageLimit());
  EXPECT_EQ(cannot.count(), 1); // latched background path

  EXPECT_FALSE(mgr.enforceStorageLimit(false, true));
  EXPECT_FALSE(mgr.enforceStorageLimit(false, true));
  EXPECT_FALSE(mgr.enforceStorageLimit(false, true));
  EXPECT_EQ(cannot.count(), 4); // every explicit attempt notifies
}

TEST_F(StorageManagerTest, RefreshEmitsSignalOnSizeChange) {
  StorageManager mgr(tempDir.path());

  // Track the usedSizeChanged signal
  QSignalSpy spy(&mgr, &StorageManager::usedSizeChanged);

  // 1. Initial refresh (no files) -> size 0
  mgr.refresh();
  // Signal might be emitted if initial size != 0, but here it's 0.
  // Let's create a file to FORCE a change.

  createDummyFile("new_rec.mkv", 1024 * 1024); // 1 MB

  // 2. Refresh again -> size changes from 0 to 1MB
  mgr.refresh();

  // Should have emitted at least once
  EXPECT_GT(spy.count(), 0);

  // 3. Refresh again without changes -> NO signal should be emitted
  spy.clear();
  mgr.refresh();
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(StorageManagerTest, RenameKeepsTimestampSuffix) {
  createDummyFile("Old_Game_2026-01-15_20-30-00.mkv", 1024);
  StorageManager mgr(tempDir.path());

  QSignalSpy spy(&mgr, &StorageManager::recordingRenamed);
  QString newPath = mgr.renameRecording(
      tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv", "New Title");

  EXPECT_FALSE(newPath.isEmpty());
  EXPECT_EQ(newPath, tempDir.path() + "/New_Title_2026-01-15_20-30-00.mkv");
  EXPECT_TRUE(QFile::exists(newPath));
  EXPECT_FALSE(
      QFile::exists(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv"));
  ASSERT_EQ(spy.count(), 1);
}

TEST_F(StorageManagerTest, RenameSanitizesTitle) {
  createDummyFile("Old_Game_2026-01-15_20-30-00.mkv", 1024);
  StorageManager mgr(tempDir.path());

  QString newPath =
      mgr.renameRecording(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv",
                          "Elden Ring: Nightreign!!");
  EXPECT_EQ(newPath,
            tempDir.path() + "/Elden_Ring_Nightreign_2026-01-15_20-30-00.mkv");
  EXPECT_TRUE(QFile::exists(newPath));
}

TEST_F(StorageManagerTest, RenameWithoutOldTimestampKeepsDateFile) {
  createDummyFile("plain.mkv", 1024);
  StorageManager mgr(tempDir.path());

  QString oldPath = tempDir.path() + "/plain.mkv";
  QFileInfo before(oldPath);
  QDateTime beforeModified = before.lastModified();

  QString newPath = mgr.renameRecording(oldPath, "Renamed");
  EXPECT_EQ(newPath, tempDir.path() + "/Renamed.mkv");
  EXPECT_TRUE(QFile::exists(newPath));
  // Filesystem date survives the rename (no new timestamp invented).
  EXPECT_EQ(QFileInfo(newPath).lastModified(), beforeModified);
}

TEST_F(StorageManagerTest, RenameCollisionAddsSuffix) {
  createDummyFile("Old_Game_2026-01-15_20-30-00.mkv", 1024);
  createDummyFile("Taken_2026-01-15_20-30-00.mkv", 1024);
  StorageManager mgr(tempDir.path());

  QString newPath = mgr.renameRecording(
      tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv", "Taken");
  EXPECT_EQ(newPath, tempDir.path() + "/Taken_2026-01-15_20-30-00_1.mkv");
  EXPECT_TRUE(QFile::exists(newPath));
  // The colliding target is untouched.
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/Taken_2026-01-15_20-30-00.mkv"));
}

TEST_F(StorageManagerTest, RenameMovesThumbnail) {
  createDummyFile("Old_Game_2026-01-15_20-30-00.mkv", 1024);
  QDir(tempDir.path()).mkpath(".thumbnails");
  QFile thumb(tempDir.path() + "/.thumbnails/Old_Game_2026-01-15_20-30-00.jpg");
  ASSERT_TRUE(thumb.open(QIODevice::WriteOnly));
  thumb.write(QByteArray(512, 't'));
  thumb.close();

  StorageManager mgr(tempDir.path());
  QString newPath = mgr.renameRecording(
      tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv", "New Title");

  EXPECT_FALSE(newPath.isEmpty());
  EXPECT_TRUE(QFile::exists(tempDir.path() +
                            "/.thumbnails/New_Title_2026-01-15_20-30-00.jpg"));
  EXPECT_FALSE(QFile::exists(tempDir.path() +
                             "/.thumbnails/Old_Game_2026-01-15_20-30-00.jpg"));
}

TEST_F(StorageManagerTest, RenameAndDeleteAllRecordingCaches) {
  createDummyFile("Game [1].mkv", 1024);
  createDummyFile("Game 1.mkv", 1024);
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".thumbnails"));
  const QStringList suffixes = {".jpg", ".peaks", ".strip-0.jpg",
                                ".strip-32-0.jpg", ".strip-64-63.jpg"};
  for (const QString &suffix : suffixes) {
    createDummyFile(".thumbnails/Game [1]" + suffix, 32);
    createDummyFile(".thumbnails/Game 1" + suffix, 32);
  }
  StorageManager mgr(tempDir.path());
  const QString renamed = mgr.renameRecording(tempDir.filePath("Game [1].mkv"), "New");
  ASSERT_FALSE(renamed.isEmpty());
  for (const QString &suffix : suffixes) {
    EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/Game [1]" + suffix)));
    EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/New" + suffix)));
  }
  ASSERT_TRUE(mgr.deleteRecording(renamed));
  for (const QString &suffix : suffixes) {
    EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/New" + suffix)));
    EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/Game 1" + suffix)));
  }
}

TEST_F(StorageManagerTest, DeletePreservesCachesSharedByAnotherExtension) {
  createDummyFile("Game.mkv", 1024);
  createDummyFile("Game.mp4", 1024);
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".thumbnails"));
  createDummyFile(".thumbnails/Game.strip-32-0.jpg", 32);
  StorageManager mgr(tempDir.path());
  ASSERT_TRUE(mgr.deleteRecording(tempDir.filePath("Game.mkv")));
  EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/Game.strip-32-0.jpg")));
  ASSERT_TRUE(mgr.deleteRecording(tempDir.filePath("Game.mp4")));
  EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/Game.strip-32-0.jpg")));
}

TEST_F(StorageManagerTest, RenameRejectsEmptyTitle) {
  createDummyFile("Old_Game_2026-01-15_20-30-00.mkv", 1024);
  StorageManager mgr(tempDir.path());

  EXPECT_TRUE(mgr.renameRecording(
                     tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv", "  ")
                  .isEmpty());
  EXPECT_TRUE(
      mgr.renameRecording(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv",
                          "___")
          .isEmpty());
  EXPECT_TRUE(
      QFile::exists(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv"));
}

TEST_F(StorageManagerTest, RenameRejectsOutsidePath) {
  QTemporaryDir outsideDir(tempDir.path() + "-outside-XXXXXX");
  ASSERT_TRUE(outsideDir.isValid());
  const QString outsidePath = outsideDir.path() + "/outside.mkv";
  QFile f(outsidePath);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write(QByteArray(1024, 'x'));
  f.close();

  StorageManager mgr(tempDir.path());
  EXPECT_TRUE(mgr.renameRecording(outsidePath, "Hacked").isEmpty());
  EXPECT_TRUE(QFile::exists(outsidePath)); // untouched
}

TEST_F(StorageManagerTest, RenameRejectsMissingFile) {
  StorageManager mgr(tempDir.path());
  EXPECT_TRUE(
      mgr.renameRecording(tempDir.path() + "/missing.mkv", "Ghost").isEmpty());
}

TEST_F(StorageManagerTest, RenameRefusesInProgressRecording) {
  createDummyFile("Live_2026-01-15_20-30-00.mkv", 1024);
  StorageManager mgr(tempDir.path());
  mgr.setExcludedRecordingPath(tempDir.path() +
                               "/Live_2026-01-15_20-30-00.mkv");

  EXPECT_TRUE(mgr.renameRecording(
                     tempDir.path() + "/Live_2026-01-15_20-30-00.mkv", "Other")
                  .isEmpty());
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/Live_2026-01-15_20-30-00.mkv"));
}

TEST_F(StorageManagerTest, DeleteRecordingRemovesSidecarThumbnail) {
  createDummyFile("old_game.mkv", 1024);
  QDir(tempDir.path()).mkpath(".thumbnails");
  QFile thumb(tempDir.path() + "/.thumbnails/old_game.jpg");
  ASSERT_TRUE(thumb.open(QIODevice::WriteOnly));
  thumb.write(QByteArray(512, 't'));
  thumb.close();
  ASSERT_TRUE(QFile::exists(tempDir.path() + "/.thumbnails/old_game.jpg"));

  StorageManager mgr(tempDir.path());
  EXPECT_TRUE(mgr.deleteRecording(tempDir.path() + "/old_game.mkv"));
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/old_game.mkv"));
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/old_game.jpg"));
}

TEST_F(StorageManagerTest, AutoCleanupRemovesSidecarThumbnails) {
  createDummyFile("old.mkv", 5000);
  createDummyFile("new.mkv", 5000);
  QDir(tempDir.path()).mkpath(".thumbnails");
  for (const char *base : {"old", "new"}) {
    QFile thumb(tempDir.path() + QString("/.thumbnails/%1.jpg").arg(base));
    ASSERT_TRUE(thumb.open(QIODevice::WriteOnly));
    thumb.write(QByteArray(512, 't'));
    thumb.close();
    createDummyFile(QString(".thumbnails/%1.strip-32-0.jpg").arg(base), 32);
    createDummyFile(QString(".thumbnails/%1.peaks").arg(base), 32);
  }

  StorageManager mgr(tempDir.path());
  mgr.setMaxSize(6000);
  mgr.setAutoDeleteOldest(true);
  mgr.enforceStorageLimit();

  QDir dir(tempDir.path());
  EXPECT_LE(dir.entryList({"*.mkv"}, QDir::Files).size(), 1);
  // At most one thumbnail + one strip frame survive, for the remaining video.
  EXPECT_LE(QDir(tempDir.path() + "/.thumbnails")
                .entryList({"*.jpg"}, QDir::Files)
                .size(),
            2);
  for (const QString &base : {QString("old"), QString("new")}) {
    const bool videoExists = QFile::exists(tempDir.filePath(base + ".mkv"));
    EXPECT_EQ(QFile::exists(tempDir.filePath(".thumbnails/" + base + ".strip-32-0.jpg")), videoExists);
    EXPECT_EQ(QFile::exists(tempDir.filePath(".thumbnails/" + base + ".peaks")), videoExists);
  }
}

TEST_F(StorageManagerTest, BulkDeleteRemovesFilesInBackground) {
  QStringList paths;
  for (int i = 0; i < 5; ++i) {
    const QString name = QString("bulk_%1.mkv").arg(i);
    createDummyFile(name, 1024);
    paths << tempDir.path() + "/" + name;
  }
  QDir(tempDir.path()).mkpath(".thumbnails");
  QFile thumb(tempDir.path() + "/.thumbnails/bulk_0.jpg");
  ASSERT_TRUE(thumb.open(QIODevice::WriteOnly));
  thumb.write(QByteArray(512, 't'));
  thumb.close();
  createDummyFile(".thumbnails/bulk_0.strip-32-0.jpg", 32);
  createDummyFile(".thumbnails/bulk_0.strip-0.jpg", 32);
  createDummyFile(".thumbnails/bulk_0.peaks", 32);

  StorageManager mgr(tempDir.path());
  QSignalSpy startedSpy(&mgr, &StorageManager::bulkDeleteStarted);
  QSignalSpy progressSpy(&mgr, &StorageManager::bulkDeleteProgress);
  QSignalSpy deletedSpy(&mgr, &StorageManager::recordingsDeleted);
  QSignalSpy finishedSpy(&mgr, &StorageManager::bulkDeleteFinished);

  EXPECT_FALSE(mgr.bulkDeleteInProgress());
  mgr.deleteRecordings(paths);
  // Returns immediately; the work happens on the thread pool.
  EXPECT_TRUE(mgr.bulkDeleteInProgress());

  ASSERT_TRUE(finishedSpy.wait(5000));
  EXPECT_FALSE(mgr.bulkDeleteInProgress());
  EXPECT_EQ(startedSpy.count(), 1);
  ASSERT_EQ(deletedSpy.count(), 1);
  EXPECT_EQ(deletedSpy.first().first().toStringList().size(), 5);
  ASSERT_EQ(finishedSpy.count(), 1);
  EXPECT_EQ(finishedSpy.first().at(0).toInt(), 5);
  EXPECT_EQ(finishedSpy.first().at(1).toInt(), 5);
  EXPECT_FALSE(progressSpy.isEmpty());

  for (const QString &p : paths) {
    EXPECT_FALSE(QFile::exists(p));
  }
  EXPECT_FALSE(QFile::exists(tempDir.path() + "/.thumbnails/bulk_0.jpg"));
  EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/bulk_0.strip-32-0.jpg")));
  EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/bulk_0.strip-0.jpg")));
  EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/bulk_0.peaks")));
  // Single refresh on completion: size settled back to zero.
  EXPECT_EQ(mgr.usedSize(), 0);
}

TEST_F(StorageManagerTest, BulkDeleteRefusesOutsidePath) {
  // Same sacrificial-sibling-dir guard as DeleteRecordingOutsidePath.
  QTemporaryDir outsideDir(tempDir.path() + "-outside-XXXXXX");
  ASSERT_TRUE(outsideDir.isValid());
  const QString outsidePath = outsideDir.path() + "/outside.mkv";
  {
    QFile f(outsidePath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(1024, 'x'));
  }
  createDummyFile("inside.mkv", 1024);

  StorageManager mgr(tempDir.path());
  QSignalSpy deletedSpy(&mgr, &StorageManager::recordingsDeleted);
  QSignalSpy finishedSpy(&mgr, &StorageManager::bulkDeleteFinished);

  mgr.deleteRecordings({tempDir.path() + "/inside.mkv", outsidePath});
  ASSERT_TRUE(finishedSpy.wait(5000));

  EXPECT_FALSE(QFile::exists(tempDir.path() + "/inside.mkv"));
  EXPECT_TRUE(QFile::exists(outsidePath));
  ASSERT_EQ(finishedSpy.count(), 1);
  EXPECT_EQ(finishedSpy.first().at(0).toInt(), 1);
  EXPECT_EQ(finishedSpy.first().at(1).toInt(), 2);
  ASSERT_EQ(deletedSpy.count(), 1);
  EXPECT_EQ(deletedSpy.first().first().toStringList(),
            QStringList({tempDir.path() + "/inside.mkv"}));
}

TEST_F(StorageManagerTest, BulkDeleteEmptyListIsNoOp) {
  StorageManager mgr(tempDir.path());
  QSignalSpy finishedSpy(&mgr, &StorageManager::bulkDeleteFinished);
  QSignalSpy startedSpy(&mgr, &StorageManager::bulkDeleteStarted);

  mgr.deleteRecordings({});

  EXPECT_FALSE(mgr.bulkDeleteInProgress());
  EXPECT_FALSE(startedSpy.wait(250));
  EXPECT_EQ(finishedSpy.count(), 0);
}
