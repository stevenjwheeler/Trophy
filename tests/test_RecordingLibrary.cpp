/*
 * Trophy - Unit Tests
 * RecordingLibrary: directory scanning, search filter, sort orders,
 * active-recording exclusion, deletion.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>

#include "library/Recording.h"
#include "library/RecordingLibrary.h"

using namespace TrophyBackend;

class RecordingLibraryTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new RecordingLibraryTestEnvironment);

class RecordingLibraryTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  void TearDown() override {
    // Recordings probe on the global thread pool; join before the
    // temp dir (and its files) goes away.
    QThreadPool::globalInstance()->waitForDone(5000);
  }

  QString createFile(const QString &name, qint64 size = 8192) {
    QString path = tempDir.path() + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(size, '\0'));
    f.close();
    return path;
  }

  // Render a real 1s clip with an embedded container title using the
  // system ffmpeg. Empty string when ffmpeg is unavailable (callers
  // should SKIP).
  QString makeTitledClip(const QString &name, const QString &title) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    proc.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                          "testsrc=size=320x240:rate=30:duration=1", "-pix_fmt",
                          "yuv420p", "-metadata",
                          QString("title=%1").arg(title), path});
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }

  void waitForProbes() { QThreadPool::globalInstance()->waitForDone(10000); }
};

TEST_F(RecordingLibraryTest, ScansDirectoryForVideos) {
  createFile("GameA_2026-01-15_20-30-00.mkv");
  createFile("GameB_2026-01-16_20-30-00.mp4");
  createFile("notes.txt"); // ignored: not a video extension

  RecordingLibrary lib(tempDir.path());
  EXPECT_EQ(lib.count(), 2);
}

TEST_F(RecordingLibraryTest, SearchFiltersByGameName) {
  createFile("Cyberpunk_2077_2026-01-15_20-30-00.mkv");
  createFile("Hades_2026-01-16_20-30-00.mkv");

  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);
  lib.setSearchQuery("cyber");
  EXPECT_EQ(lib.count(), 1);
  lib.setSearchQuery("");
  EXPECT_EQ(lib.count(), 2);
}

TEST_F(RecordingLibraryTest, SortOrdersChangeSequence) {
  const QString a = createFile("A_Game_2026-01-15_20-30-00.mkv", 8192);
  const QString b = createFile("B_Game_2026-01-16_20-30-00.mkv", 16384);

  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  lib.setSortOrder(RecordingLibrary::NameAZ);
  ASSERT_GE(lib.count(), 2);
  EXPECT_LT(lib.get(0)->fileName().compare(lib.get(1)->fileName()), 0);

  lib.setSortOrder(RecordingLibrary::LargestFirst);
  EXPECT_GT(lib.get(0)->size(), lib.get(1)->size());
  EXPECT_EQ(lib.get(0)->filePath(), b);
  EXPECT_EQ(lib.get(1)->filePath(), a);
}

TEST_F(RecordingLibraryTest, ActiveRecordingExcludedFromCount) {
  const QString path = createFile("Live_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);

  lib.setActiveRecordingPath(path);
  EXPECT_EQ(lib.count(), 0);

  lib.setActiveRecordingPath(QString());
  EXPECT_EQ(lib.count(), 1);
}

TEST_F(RecordingLibraryTest, DeleteRecordingRemovesFileAndEntry) {
  const QString path = createFile("Doomed_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);

  QSignalSpy removedSpy(&lib, &RecordingLibrary::recordingRemoved);
  lib.deleteRecording(path);

  EXPECT_FALSE(QFile::exists(path));
  EXPECT_EQ(removedSpy.count(), 1);
}

TEST_F(RecordingLibraryTest, GetRecordingByPath) {
  const QString path = createFile("Found_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());

  Recording *rec = lib.getRecording(path);
  ASSERT_NE(rec, nullptr);
  EXPECT_EQ(rec->filePath(), path);
  EXPECT_EQ(lib.getRecording(tempDir.path() + "/missing.mkv"), nullptr);
}

TEST_F(RecordingLibraryTest, RefreshPicksUpNewFiles) {
  RecordingLibrary lib(tempDir.path());
  EXPECT_EQ(lib.count(), 0);

  QSignalSpy addedSpy(&lib, &RecordingLibrary::recordingAdded);
  createFile("Late_2026-01-15_20-30-00.mkv");
  lib.refresh();

  EXPECT_EQ(lib.count(), 1);
  EXPECT_EQ(addedSpy.count(), 1);
}

TEST_F(RecordingLibraryTest, RefreshAfterFilesystemRenameShowsNewName) {
  createFile("Old_Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);
  const QDateTime dateBefore = lib.get(0)->date();

  ASSERT_TRUE(QFile::rename(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv",
                            tempDir.path() + "/New_Title_2026-01-15_20-30-00.mkv"));
  lib.refresh();

  ASSERT_EQ(lib.count(), 1);
  EXPECT_EQ(lib.get(0)->fileName(),
            "New_Title_2026-01-15_20-30-00.mkv");
  EXPECT_EQ(lib.get(0)->gameName(), "New Title");
  // Filesystem rename preserves the recording date.
  EXPECT_EQ(lib.get(0)->date(), dateBefore);
}

TEST_F(RecordingLibraryTest, RenamedNameSurvivesStaleContainerTitle) {
  // Regression: Trophy bakes the game name into the container title tag
  // at record time, so after an in-app rename the background probe kept
  // reverting the displayed name to the stale tag.
  const QString oldPath =
      makeTitledClip("Old_Game_2026-01-15_20-30-00.mkv", "Old Game");
  if (oldPath.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  RecordingLibrary lib(tempDir.path());
  waitForProbes();
  ASSERT_EQ(lib.count(), 1);
  ASSERT_EQ(lib.get(0)->gameName(), "Old Game");

  // Simulate the app rename flow: filesystem rename (StorageManager) +
  // rename notification + refresh.
  const QString newPath = tempDir.path() + "/New_Title_2026-01-15_20-30-00.mkv";
  ASSERT_TRUE(QFile::rename(oldPath, newPath));
  lib.onRecordingRenamed(oldPath, newPath);
  lib.refresh();
  waitForProbes();

  ASSERT_EQ(lib.count(), 1);
  EXPECT_EQ(lib.get(0)->fileName(), "New_Title_2026-01-15_20-30-00.mkv");
  EXPECT_EQ(lib.get(0)->gameName(), "New Title");
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/.trophy-titles.json"));

  // Restart simulation: a fresh library on the same directory must keep
  // the renamed title (preference is persisted, not just in-memory).
  RecordingLibrary relaunched(tempDir.path());
  waitForProbes();
  ASSERT_EQ(relaunched.count(), 1);
  EXPECT_EQ(relaunched.get(0)->gameName(), "New Title");
}
