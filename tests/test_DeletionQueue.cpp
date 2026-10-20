#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QThread>
#include "core/DeletionQueue.h"
#include "core/StorageManager.h"
#include "library/RecordingAnnotations.h"
using namespace TrophyBackend;
class DeletionEnvironment : public testing::Environment {
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1; static char arg[] = "test"; static char *argv[]{arg, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};
static auto *environment = testing::AddGlobalTestEnvironment(new DeletionEnvironment);
class DeletionQueueTest : public testing::Test {
protected:
  QTemporaryDir directory;
  qint64 now = 0;
  void clock(DeletionQueue &queue) { queue.m_now = [this] { return now; }; }
  void tick(DeletionQueue &queue) { queue.tick(); }
  QString file(const QString &name) {
    const QString path = directory.filePath(name);
    QFile f(path); EXPECT_TRUE(f.open(QIODevice::WriteOnly)); f.write(QByteArray(128, 'a')); f.close();
    return path;
  }
  bool settle(DeletionQueue &queue) {
    QElapsedTimer deadline; deadline.start();
    while (!queue.pending().isEmpty() && deadline.elapsed() < 3000) {
      QCoreApplication::processEvents(); QThread::msleep(1);
    }
    return queue.pending().isEmpty();
  }
};
TEST_F(DeletionQueueTest, UndoKeepsVideoMetadataAndStorageAccounting) {
  const auto path = file("Game.mkv");
  ASSERT_TRUE(RecordingAnnotations::setProtected(path, true));
  ASSERT_TRUE(RecordingAnnotations::addBookmark(path, 12345));
  StorageManager storage(directory.path());
  DeletionQueue queue(&storage); clock(queue);
  QSignalSpy changed(&queue, &DeletionQueue::pendingChanged);
  QSignalSpy hidden(&queue, &DeletionQueue::hiddenPathsChanged);
  ASSERT_TRUE(queue.enqueue({path}));
  const auto id = queue.pending()[0].toMap()["id"].toString();
  EXPECT_TRUE(QFile::exists(path));
  EXPECT_EQ(storage.usedSize(), 128);
  EXPECT_EQ(hidden.last()[0].toStringList(), QStringList{path});
  EXPECT_FALSE(storage.deleteRecording(path));
  now = 5000; tick(queue);
  EXPECT_DOUBLE_EQ(queue.status(id)["progress"].toDouble(), 0.5);
  EXPECT_EQ(changed.count(), 1); // Progress never rebuilds the toast's Undo button.
  ASSERT_TRUE(queue.undo(id));
  EXPECT_TRUE(queue.pending().isEmpty());
  EXPECT_TRUE(hidden.last()[0].toStringList().isEmpty());
  EXPECT_TRUE(RecordingAnnotations::isProtected(path));
  EXPECT_EQ(RecordingAnnotations::bookmarks(path).size(), 1);
  ASSERT_TRUE(storage.reserveRecordings({path}));
  storage.releaseRecordings({path});
}
TEST_F(DeletionQueueTest, ExpiryDeletesOnlyAfterDeadlineIncludingAnnotations) {
  const auto path = file("Game.mkv");
  ASSERT_TRUE(RecordingAnnotations::addBookmark(path, 42));
  StorageManager storage(directory.path());
  DeletionQueue queue(&storage); clock(queue);
  ASSERT_TRUE(queue.enqueue({path}));
  now = 9999; tick(queue); EXPECT_TRUE(QFile::exists(path));
  now = 10000; tick(queue); ASSERT_TRUE(settle(queue));
  EXPECT_FALSE(QFile::exists(path));
  EXPECT_FALSE(QFile::exists(RecordingAnnotations::path(path)));
  EXPECT_EQ(storage.usedSize(), 0);
}
TEST_F(DeletionQueueTest, ShutdownCancelsGracePeriodAndReleasesReservations) {
  const auto path = file("Game.mkv");
  StorageManager storage(directory.path());
  { DeletionQueue queue(&storage); clock(queue); ASSERT_TRUE(queue.enqueue({path})); }
  EXPECT_TRUE(QFile::exists(path));
  EXPECT_TRUE(storage.reserveRecordings({path}));
  storage.releaseRecordings({path});
}
TEST_F(DeletionQueueTest, IndependentBatchesDoNotReplaceEachOther) {
  const auto first = file("First.mkv"), second = file("Second.mkv");
  StorageManager storage(directory.path());
  DeletionQueue queue(&storage); clock(queue);
  ASSERT_TRUE(queue.enqueue({first}));
  now = 5000; ASSERT_TRUE(queue.enqueue({second}));
  ASSERT_EQ(queue.pending().size(), 2);
  const auto secondId = queue.pending()[1].toMap()["id"].toString();
  now = 10000; tick(queue);
  ASSERT_TRUE(queue.undo(secondId));
  ASSERT_TRUE(settle(queue));
  EXPECT_FALSE(QFile::exists(first)); EXPECT_TRUE(QFile::exists(second));
}
TEST_F(DeletionQueueTest, ChangedFileIsNeverDeletedAsTheOldCapture) {
  const auto path = file("Game.mkv");
  StorageManager storage(directory.path());
  DeletionQueue queue(&storage); clock(queue);
  QSignalSpy failure(&queue, &DeletionQueue::failed);
  ASSERT_TRUE(queue.enqueue({path}));
  ASSERT_TRUE(QFile::remove(path));
  QFile replacement(path); ASSERT_TRUE(replacement.open(QIODevice::WriteOnly));
  replacement.write("replacement"); replacement.close();
  now = 10000; tick(queue);
  EXPECT_TRUE(queue.pending().isEmpty()); EXPECT_EQ(failure.count(), 1);
  EXPECT_TRUE(QFile::exists(path));
}
TEST_F(DeletionQueueTest, PendingFilesCannotBeAutoCleanedOrReservedForExport) {
  const auto path = file("Game.mkv");
  StorageManager storage(directory.path());
  DeletionQueue queue(&storage); clock(queue);
  ASSERT_TRUE(queue.enqueue({path}));
  EXPECT_FALSE(storage.reserveRecordings({path}));
  storage.setMaxSize(1);
  EXPECT_FALSE(storage.enforceStorageLimit(true));
  EXPECT_TRUE(QFile::exists(path));
  EXPECT_FALSE(queue.enqueue({directory.filePath("missing.mkv")}));
}
