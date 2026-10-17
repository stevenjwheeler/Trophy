/*
 * Trophy - Unit Tests
 * RecordingLibrary: directory scanning, search filter, sort orders,
 * active-recording exclusion, deletion.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>

#include "core/StorageManager.h"
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

  // Render a real clip with an embedded container title using the
  // system ffmpeg. Empty string when ffmpeg is unavailable (callers
  // should SKIP).
  QString makeTitledClip(const QString &name, const QString &title,
                         int durationSec = 1, const QString &kind = {},
                         const QString &date = "2026-01-01T12:00:00Z") {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    QStringList args = {"-y", "-v", "error", "-f", "lavfi", "-i",
         QString("testsrc=size=320x240:rate=30:duration=%1").arg(durationSec),
         "-pix_fmt", "yuv420p",
         // Fixed GOP: real recordings have regular
         // keyframes; the encoder default here writes a
         // single head keyframe, which nothing can cut.
         "-g", "12", "-keyint_min", "12", "-metadata",
          QString("title=%1").arg(title), "-metadata",
          QString("creation_time=%1").arg(date)};
    if (!kind.isEmpty())
      args << "-metadata" << QString("trophy-kind=%1").arg(kind);
    args << path;
    proc.start("ffmpeg", args);
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }

  void waitForProbes() {
    QThreadPool::globalInstance()->waitForDone(10000);
    QCoreApplication::processEvents();
  }
};

TEST_F(RecordingLibraryTest, ScanRemovesOrphanCachesButKeepsLiveAndUnknownFiles) {
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".thumbnails"));
  const QStringList suffixes = {".jpg", ".peaks", ".strip-0.jpg", ".strip-32-0.jpg"};
  createFile("Live.mkv");
  for (const QString &suffix : suffixes) {
    createFile(".thumbnails/Live" + suffix);
    createFile(".thumbnails/Deleted" + suffix);
  }
  createFile(".thumbnails/notes.txt");
  createFile(".thumbnails/Deleted.strip-oops.jpg.bak");
  RecordingLibrary lib(tempDir.path());
  for (const QString &suffix : suffixes) {
    EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/Live" + suffix)));
    EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/Deleted" + suffix)));
  }
  EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/notes.txt")));
  EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/Deleted.strip-oops.jpg.bak")));
  ASSERT_TRUE(QFile::remove(tempDir.filePath("Live.mkv")));
  lib.refresh();
  for (const QString &suffix : suffixes)
    EXPECT_FALSE(QFile::exists(tempDir.filePath(".thumbnails/Live" + suffix)));
}

TEST_F(RecordingLibraryTest, OrphanSweepDoesNotFollowSymlinkedCacheDirectory) {
  QTemporaryDir outside;
  ASSERT_TRUE(outside.isValid());
  QFile file(outside.filePath("Deleted.strip-32-0.jpg"));
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write("cache");
  file.close();
  ASSERT_TRUE(QFile::link(outside.path(), tempDir.filePath(".thumbnails")));
  RecordingLibrary lib(tempDir.path());
  EXPECT_TRUE(QFile::exists(file.fileName()));
}

TEST_F(RecordingLibraryTest, OrphanSweepPreservesAmbiguousStripNamedVideoThumbnail) {
  ASSERT_TRUE(QDir(tempDir.path()).mkpath(".thumbnails"));
  createFile("Game.strip-32-0.mkv");
  createFile(".thumbnails/Game.strip-32-0.jpg");
  RecordingLibrary lib(tempDir.path());
  EXPECT_TRUE(QFile::exists(tempDir.filePath(".thumbnails/Game.strip-32-0.jpg")));
}

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

TEST_F(RecordingLibraryTest, KindFilterCombinesWithSearchAndActiveExclusion) {
  const auto clip = createFile("Hades_2026-01-15_20-30-00_Replay.mkv");
  createFile("Hades_2026-01-16_20-30-00.mkv");
  createFile("Portal_2026-01-16_20-30-00_Replay.mkv");
  RecordingLibrary lib(tempDir.path());
  lib.setKindFilter(RecordingLibrary::ClipsOnly);
  EXPECT_EQ(lib.count(), 2);
  lib.setSearchQuery("hades");
  ASSERT_EQ(lib.count(), 1);
  EXPECT_EQ(lib.get(0)->filePath(), clip);
  lib.setActiveRecordingPath(clip);
  EXPECT_EQ(lib.count(), 0);
  lib.setActiveRecordingPath({});
  lib.setKindFilter(RecordingLibrary::RecordingsOnly);
  ASSERT_EQ(lib.count(), 1);
  EXPECT_FALSE(lib.get(0)->isClip());
  lib.setSearchQuery({});
  lib.setKindFilter(RecordingLibrary::AllCaptures);
  EXPECT_EQ(lib.count(), 3);
  lib.setKindFilter(RecordingLibrary::NoCaptures);
  EXPECT_EQ(lib.count(), 0);
  lib.setKindFilter(RecordingLibrary::AllCaptures);
  EXPECT_EQ(lib.count(), 3);
}

TEST_F(RecordingLibraryTest, AdditionalSortOrdersAndInvalidValues) {
  const auto a = createFile("Alpha.mkv", 8192);
  const auto b = createFile("Beta.mkv", 16384);
  RecordingLibrary lib(tempDir.path());
  lib.setSortOrder(RecordingLibrary::SmallestFirst);
  EXPECT_EQ(lib.get(0)->filePath(), a);
  lib.setSortOrder(RecordingLibrary::NameZA);
  EXPECT_EQ(lib.get(0)->filePath(), b);
  QSignalSpy changed(&lib, &RecordingLibrary::recordingsChanged);
  lib.setSortOrder(-1);
  lib.setSortOrder(999);
  lib.setKindFilter(-1);
  lib.setKindFilter(999);
  EXPECT_EQ(lib.sortOrder(), RecordingLibrary::NameZA);
  EXPECT_EQ(lib.kindFilter(), RecordingLibrary::AllCaptures);
  EXPECT_EQ(changed.count(), 0);
}

TEST_F(RecordingLibraryTest, MetadataCompletionReappliesDurationDateAndKind) {
  const auto shortPath = makeTitledClip("Renamed.mkv", "Zulu", 1, "clip",
                                       "2026-01-02T12:00:00Z");
  const auto longPath = makeTitledClip("Other.mkv", "Alpha", 3);
  if (shortPath.isEmpty() || longPath.isEmpty())
    GTEST_SKIP() << "ffmpeg fixture unavailable";

  RecordingLibrary lib(tempDir.path());
  // Apply before queued probe results; a renamed clip has no suffix.
  lib.setKindFilter(RecordingLibrary::ClipsOnly);
  lib.setSortOrder(RecordingLibrary::LongestFirst);
  waitForProbes();
  ASSERT_EQ(lib.count(), 1);
  EXPECT_EQ(lib.get(0)->filePath(), shortPath);
  lib.setKindFilter(RecordingLibrary::AllCaptures);
  ASSERT_EQ(lib.count(), 2);
  EXPECT_EQ(lib.get(0)->filePath(), longPath);
  EXPECT_GT(lib.get(0)->duration(), lib.get(1)->duration());
  lib.setSortOrder(RecordingLibrary::ShortestFirst);
  EXPECT_EQ(lib.get(0)->filePath(), shortPath);
  lib.setSortOrder(RecordingLibrary::NewestFirst);
  EXPECT_EQ(lib.get(0)->filePath(), shortPath);
  lib.setSortOrder(RecordingLibrary::OldestFirst);
  EXPECT_EQ(lib.get(0)->filePath(), longPath);
  lib.setSortOrder(RecordingLibrary::NameAZ);
  EXPECT_EQ(lib.get(0)->filePath(), longPath);
  lib.setSortOrder(RecordingLibrary::NameZA);
  EXPECT_EQ(lib.get(0)->filePath(), shortPath);
}

TEST_F(RecordingLibraryTest, EqualSortValuesUseStablePathOrder) {
  const auto b = createFile("Beta.mkv");
  const auto a = createFile("Alpha.mkv");
  RecordingLibrary lib(tempDir.path());
  for (const auto sort : {RecordingLibrary::LargestFirst,
                          RecordingLibrary::SmallestFirst,
                          RecordingLibrary::LongestFirst,
                          RecordingLibrary::ShortestFirst}) {
    lib.setSortOrder(sort);
    EXPECT_EQ(lib.get(0)->filePath(), a);
    EXPECT_EQ(lib.get(1)->filePath(), b);
  }
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

TEST_F(RecordingLibraryTest, RenameDoesNotEmitRemoved) {
  const QString oldPath = createFile("Old_Name_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);

  // A user rename moves old -> new on disk; the refresh that follows
  // must not report the source as a deletion (it showed up as a
  // phantom "Deleted" activity).
  QSignalSpy removedSpy(&lib, &RecordingLibrary::recordingRemoved);
  const QString newPath = tempDir.path() + "/New_Name_2026-01-15_20-30-00.mkv";
  ASSERT_TRUE(QFile::rename(oldPath, newPath));
  lib.onRecordingRenamed(oldPath, newPath);
  lib.refresh();

  EXPECT_EQ(removedSpy.count(), 0);
  EXPECT_EQ(lib.count(), 1);
  EXPECT_NE(lib.getRecording(newPath), nullptr);
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

TEST_F(RecordingLibraryTest, DeleteRecordingRejectsSiblingPrefixPath) {
  QTemporaryDir sibling(tempDir.path() + "-outside-XXXXXX");
  ASSERT_TRUE(sibling.isValid());
  const QString outsidePath = sibling.path() + "/outside.mkv";
  {
    QFile outside(outsidePath);
    ASSERT_TRUE(outside.open(QIODevice::WriteOnly));
    outside.write("outside");
  }

  RecordingLibrary lib(tempDir.path());
  lib.deleteRecording(outsidePath);
  EXPECT_TRUE(QFile::exists(outsidePath));
}

TEST_F(RecordingLibraryTest, FilteredListSettledBeforeRemovedSignal) {
  createFile("Keep_2026-01-15_20-30-00.mkv");
  const QString doomed = createFile("Doomed_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  // Regression (live SIGSEGV 2026-10-01): removeRecording() emitted
  // recordingRemoved while m_filteredRecordings still held the freed
  // Recording, so an observer calling get() in the slot (the dashboard
  // activity feed does) wrapped a dangling QObject and crashed the QML
  // engine in QV4::fromData.
  bool sawDoomed = false;
  int slotCount = -1;
  QObject::connect(&lib, &RecordingLibrary::recordingRemoved, &lib,
                   [&](const QString &removed) {
                     EXPECT_EQ(removed, doomed);
                     slotCount = lib.count();
                     for (int i = 0; i < lib.count(); ++i) {
                       Recording *r = lib.get(i);
                       ASSERT_NE(r, nullptr);
                       if (r->filePath() == doomed)
                         sawDoomed = true;
                     }
                   });
  lib.removeRecording(doomed);

  EXPECT_FALSE(sawDoomed);
  EXPECT_EQ(slotCount, 1);
  EXPECT_EQ(lib.count(), 1);
}

TEST_F(RecordingLibraryTest, FilteredListSettledBeforeAddedSignal) {
  RecordingLibrary lib(tempDir.path());

  QString seen;
  bool foundInSlot = false;
  QObject::connect(&lib, &RecordingLibrary::recordingAdded, &lib,
                   [&](Recording *rec) {
                     seen = rec ? rec->filePath() : QString();
                     for (int i = 0; i < lib.count(); ++i) {
                       if (lib.get(i) == rec) {
                         foundInSlot = true;
                         break;
                       }
                     }
                   });
  const QString path = createFile("Fresh_2026-01-15_20-30-00.mkv");
  lib.refresh();

  EXPECT_EQ(seen, path);
  EXPECT_TRUE(foundInSlot);
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

TEST_F(RecordingLibraryTest, ScanBatchesAdditionsAndRemovalsIntoOneModelChange) {
  const QString removedA = createFile("RemovedA.mkv");
  const QString removedB = createFile("RemovedB.mp4");
  const QString keep = createFile("Keep.webm");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 3);

  ASSERT_TRUE(QFile::remove(removedA));
  ASSERT_TRUE(QFile::remove(removedB));
  const QString addedA = createFile("AddedA.mkv");
  const QString addedB = createFile("AddedB.mp4");
  const QString addedC = createFile("AddedC.webm");
  QSignalSpy changed(&lib, &RecordingLibrary::recordingsChanged);
  QSignalSpy added(&lib, &RecordingLibrary::recordingAdded);
  QSignalSpy removed(&lib, &RecordingLibrary::recordingRemoved);

  // No event-loop pumping: measure this synchronous scan, not later
  // metadata/thumbnail deliveries or platform watcher event counts.
  lib.refresh();

  EXPECT_EQ(changed.count(), 1);
  EXPECT_EQ(added.count(), 3);
  EXPECT_EQ(removed.count(), 2);
  EXPECT_EQ(lib.count(), 4);
  for (const QString &path : {keep, addedA, addedB, addedC})
    EXPECT_NE(lib.getRecording(path), nullptr);
  EXPECT_EQ(lib.getRecording(removedA), nullptr);
  EXPECT_EQ(lib.getRecording(removedB), nullptr);

  changed.clear();
  added.clear();
  removed.clear();
  lib.refresh();
  EXPECT_EQ(changed.count(), 1); // No-op refresh also settles once.
  EXPECT_EQ(added.count(), 0);
  EXPECT_EQ(removed.count(), 0);
}

TEST_F(RecordingLibraryTest, DeleteWatcherEchoDoesNotSettleModelAgain) {
  const QString doomed = createFile("Doomed.mkv");
  const QString keep = createFile("Keep.mkv");
  RecordingLibrary lib(tempDir.path());
  lib.deleteRecording(doomed);
  ASSERT_EQ(lib.count(), 1);
  QSignalSpy changed(&lib, &RecordingLibrary::recordingsChanged);

  ASSERT_TRUE(QMetaObject::invokeMethod(
      &lib, "onDirectoryChanged", Qt::DirectConnection,
      Q_ARG(QString, tempDir.path())));

  EXPECT_EQ(changed.count(), 0);
  EXPECT_NE(lib.getRecording(keep), nullptr);
}

TEST_F(RecordingLibraryTest, DeleteSuppressionStillRefreshesUnrelatedChanges) {
  const QString selfDeleted = createFile("SelfDeleted.mkv");
  const QString externallyDeleted = createFile("ExternallyDeleted.mkv");
  const QString keep = createFile("Keep.mkv");
  RecordingLibrary lib(tempDir.path());

  lib.deleteRecording(selfDeleted);
  ASSERT_EQ(lib.count(), 2);
  ASSERT_TRUE(QFile::remove(externallyDeleted));
  const QString unrelated = createFile("Unrelated.mp4");
  QSignalSpy changed(&lib, &RecordingLibrary::recordingsChanged);
  QSignalSpy added(&lib, &RecordingLibrary::recordingAdded);
  QSignalSpy removed(&lib, &RecordingLibrary::recordingRemoved);

  // Deliver the same-directory burst directly while the self-delete
  // suppression is still armed. Unrelated additions AND removals must
  // survive; a global skip-next-event flag would lose both.
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &lib, "onDirectoryChanged", Qt::DirectConnection,
      Q_ARG(QString, tempDir.path())));

  EXPECT_EQ(changed.count(), 1);
  ASSERT_EQ(added.count(), 1);
  ASSERT_EQ(removed.count(), 1);
  EXPECT_EQ(removed.first().first().toString(), externallyDeleted);
  EXPECT_EQ(lib.count(), 2);
  EXPECT_NE(lib.getRecording(unrelated), nullptr);
  EXPECT_NE(lib.getRecording(keep), nullptr);
  EXPECT_EQ(lib.getRecording(selfDeleted), nullptr);
  EXPECT_EQ(lib.getRecording(externallyDeleted), nullptr);
}

TEST_F(RecordingLibraryTest, RefreshAfterFilesystemRenameShowsNewName) {
  createFile("Old_Game_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);
  const QDateTime dateBefore = lib.get(0)->date();

  ASSERT_TRUE(
      QFile::rename(tempDir.path() + "/Old_Game_2026-01-15_20-30-00.mkv",
                    tempDir.path() + "/New_Title_2026-01-15_20-30-00.mkv"));
  lib.refresh();

  ASSERT_EQ(lib.count(), 1);
  EXPECT_EQ(lib.get(0)->fileName(), "New_Title_2026-01-15_20-30-00.mkv");
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

TEST_F(RecordingLibraryTest, RemoveRecordingsBatchSettlesOnce) {
  const QString a = createFile("A_2026-01-15_20-30-00.mkv");
  const QString b = createFile("B_2026-01-15_20-30-00.mkv");
  const QString c = createFile("C_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 3);

  QSignalSpy changedSpy(&lib, &RecordingLibrary::recordingsChanged);
  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  QSignalSpy singleSpy(&lib, &RecordingLibrary::recordingRemoved);

  lib.removeRecordings({a, b});

  // One model settle, one batch signal, zero per-file signals: the
  // per-file storm (a full StorageManager rescan + QML rebuild each)
  // is what froze the UI on multi-select deletes.
  EXPECT_EQ(lib.count(), 1);
  EXPECT_NE(lib.getRecording(c), nullptr);
  EXPECT_EQ(singleSpy.count(), 0);
  ASSERT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(batchSpy.first().first().toStringList(), QStringList({a, b}));
  EXPECT_EQ(changedSpy.count(), 1);
}

TEST_F(RecordingLibraryTest, RemoveRecordingsIgnoresUntrackedPaths) {
  const QString a = createFile("A_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);

  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  QSignalSpy singleSpy(&lib, &RecordingLibrary::recordingRemoved);

  // Unknown paths (and empty lists) stay quiet instead of emitting
  // empty batches that downstream refreshes would chase.
  lib.removeRecordings({tempDir.path() + "/ghost.mkv"});
  lib.removeRecordings({});
  EXPECT_EQ(lib.count(), 1);
  EXPECT_EQ(batchSpy.count(), 0);
  EXPECT_EQ(singleSpy.count(), 0);

  // Mixed known + unknown reports only the known drop.
  lib.removeRecordings({a, tempDir.path() + "/ghost.mkv"});
  EXPECT_EQ(lib.count(), 0);
  ASSERT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(batchSpy.first().first().toStringList(), QStringList({a}));
}

TEST_F(RecordingLibraryTest, RemoveRecordingsWorksWhileBulkSuppressed) {
  const QString a = createFile("A_2026-01-15_20-30-00.mkv");
  const QString b = createFile("B_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  // Suppression gates watcher refreshes only; the direct batch call
  // the bulk completion path makes must still settle.
  lib.setBulkOperationActive(true);
  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  lib.removeRecordings({a});
  EXPECT_EQ(lib.count(), 1);
  ASSERT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(batchSpy.first().first().toStringList(), QStringList({a}));
  lib.setBulkOperationActive(false);
  EXPECT_EQ(lib.count(), 1);
  EXPECT_NE(lib.getRecording(b), nullptr);
}

TEST_F(RecordingLibraryTest, RemoveRecordingsSwallowsRenameSource) {
  const QString oldPath = createFile("Old_2026-01-15_20-30-00.mkv");
  const QString keep = createFile("Keep_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  // The rename is adopted in place, so the stale source path is no
  // longer tracked at all: a bulk remove listing it only drops the
  // other selection, and the adopted recording survives under its new
  // path (never reported as removed).
  const QString newPath = tempDir.path() + "/New_2026-01-15_20-30-00.mkv";
  ASSERT_TRUE(QFile::rename(oldPath, newPath));
  lib.onRecordingRenamed(oldPath, newPath);

  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  lib.removeRecordings({oldPath, keep});
  EXPECT_EQ(lib.count(), 1);
  ASSERT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(batchSpy.first().first().toStringList(), QStringList({keep}));
  EXPECT_NE(lib.getRecording(newPath), nullptr);
}

TEST_F(RecordingLibraryTest, UnadoptedRenameSourceStillSwallowed) {
  // Fallback path: the rename target is already tracked, so in-place
  // adoption is skipped and the vanished source is removed by the
  // model instead — still not a deletion for observers.
  const QString oldPath = createFile("Old_2026-01-15_20-30-00.mkv");
  const QString clashPath = createFile("New_2026-01-15_20-30-00.mkv");
  const QString keep = createFile("Keep_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 3);

  ASSERT_TRUE(QFile::rename(
      oldPath, tempDir.path() + "/MovedAside_2026-01-15_20-30-00.mkv"));
  lib.onRecordingRenamed(oldPath, clashPath);

  QSignalSpy removedSpy(&lib, &RecordingLibrary::recordingRemoved);
  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  lib.removeRecordings({oldPath, keep});
  EXPECT_EQ(lib.count(), 1);
  EXPECT_NE(lib.getRecording(clashPath), nullptr);
  EXPECT_EQ(removedSpy.count(), 0);
  ASSERT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(batchSpy.first().first().toStringList(), QStringList({keep}));
}

TEST_F(RecordingLibraryTest, UnadoptedSingleRemoveStillSwallowed) {
  // Same fallback rule on the single path: the rename target is
  // already tracked, so adoption is skipped, the source is marked,
  // and its direct removal stays silent.
  const QString oldPath = createFile("Old_2026-01-15_20-30-00.mkv");
  const QString clashPath = createFile("New_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  lib.onRecordingRenamed(oldPath, clashPath);
  QSignalSpy removedSpy(&lib, &RecordingLibrary::recordingRemoved);
  lib.removeRecording(oldPath);
  EXPECT_EQ(lib.count(), 1);
  EXPECT_NE(lib.getRecording(clashPath), nullptr);
  EXPECT_EQ(removedSpy.count(), 0);
}

TEST_F(RecordingLibraryTest, BulkDeleteWiringSettlesOnceEndToEnd) {
  // Mirrors the Application.cpp bulk chain: StorageManager worker ->
  // recordingsDeleted -> library removeRecordings, watcher suppression
  // around the whole flight, single storage refresh at the end.
  QStringList paths;
  for (int i = 0; i < 4; ++i) {
    paths << createFile(QString("Bulk_%1_2026-01-15_20-30-00.mkv").arg(i));
  }
  StorageManager storage(tempDir.path());
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 4);

  QObject::connect(&storage, &StorageManager::recordingsDeleted, &lib,
                   &RecordingLibrary::removeRecordings);
  QObject::connect(&storage, &StorageManager::bulkDeleteStarted, &lib,
                   [&lib]() { lib.setBulkOperationActive(true); });
  QObject::connect(&storage, &StorageManager::bulkDeleteFinished, &lib,
                   [&lib](int, int) {
                     lib.setBulkOperationActive(false);
                     lib.refresh();
                   });
  QObject::connect(&lib, &RecordingLibrary::recordingsRemoved, &storage,
                   &StorageManager::refresh);

  QSignalSpy batchSpy(&lib, &RecordingLibrary::recordingsRemoved);
  QSignalSpy singleSpy(&lib, &RecordingLibrary::recordingRemoved);
  QSignalSpy finishedSpy(&storage, &StorageManager::bulkDeleteFinished);

  storage.deleteRecordings(paths);
  ASSERT_TRUE(finishedSpy.wait(5000));

  for (const QString &p : paths) {
    EXPECT_FALSE(QFile::exists(p));
  }
  EXPECT_EQ(lib.count(), 0);
  // Exactly one batch notification, no per-file storm — even though
  // the watcher saw every unlink in between.
  EXPECT_EQ(batchSpy.count(), 1);
  EXPECT_EQ(singleSpy.count(), 0);
  EXPECT_EQ(storage.usedSize(), 0);
}

TEST_F(RecordingLibraryTest, SaveClipRangeRejectsBadInput) {
  RecordingLibrary lib(tempDir.path());
  QSignalSpy failed(&lib, &RecordingLibrary::clipSaveFailed);
  QSignalSpy saved(&lib, &RecordingLibrary::clipSaved);
  EXPECT_FALSE(lib.clipSaving());
  // Missing source, then a real file with degenerate ranges (validation
  // runs before anything is opened, so the zero-filled placeholder is
  // never read).
  lib.saveClipRange("/nonexistent/x.mkv", 0.0, 1.0);
  const QString some = createFile("GameA_2026-01-15_20-30-00.mkv");
  lib.saveClipRange(some, 2.0, 2.0);
  lib.saveClipRange(some, 3.0, 1.0);
  lib.saveClipRange(some, -1.0, 1.0);
  EXPECT_EQ(failed.count(), 4);
  EXPECT_EQ(saved.count(), 0);
  EXPECT_FALSE(lib.clipSaving());
}

TEST_F(RecordingLibraryTest, SaveClipRangeRefusesWhenStorageFull) {
  const QString src = createFile("GameA_2026-01-15_20-30-00.mkv", 8192);
  RecordingLibrary lib(tempDir.path());
  StorageManager storage(tempDir.path());
  storage.setMaxSize(1024); // over limit: 8192 used > 1024 max
  ASSERT_TRUE(storage.isOverLimit());
  lib.setStorageManager(&storage);

  QSignalSpy failed(&lib, &RecordingLibrary::clipSaveFailed);
  QSignalSpy saved(&lib, &RecordingLibrary::clipSaved);
  lib.saveClipRange(src, 0.0, 1.0);

  EXPECT_EQ(failed.count(), 1);
  EXPECT_EQ(saved.count(), 0);
  EXPECT_FALSE(lib.clipSaving());
  EXPECT_TRUE(failed.first().first().toString().contains("Storage"));
  // Refused before any remux thread: the library still holds only the
  // source (no clip, no .part).
  EXPECT_EQ(QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files).size(), 1);
}

TEST_F(RecordingLibraryTest, SaveClipRangeRefusesWhenClipWouldExceedLimit) {
  const QString src = createFile("GameA_2026-01-15_20-30-00.mkv", 4096);
  RecordingLibrary lib(tempDir.path());
  StorageManager storage(tempDir.path());
  // Under the limit (4096 < 6000) but the clip — ~the full 4096-byte
  // source, whose duration is unknown on a dummy file — would push
  // usage to 8192 > 6000.
  storage.setMaxSize(6000);
  ASSERT_FALSE(storage.isOverLimit());
  lib.setStorageManager(&storage);

  QSignalSpy failed(&lib, &RecordingLibrary::clipSaveFailed);
  QSignalSpy saved(&lib, &RecordingLibrary::clipSaved);
  lib.saveClipRange(src, 0.0, 1.0);

  EXPECT_EQ(failed.count(), 1);
  EXPECT_EQ(saved.count(), 0);
  EXPECT_FALSE(lib.clipSaving());
  EXPECT_EQ(QDir(tempDir.path()).entryList({"*.mkv"}, QDir::Files).size(), 1);
}

TEST_F(RecordingLibraryTest, SaveClipRangeCutsRealFile) {
  // 5s source: the single-keyframe 1s file can't be cut at all (its
  // only keyframe is the DTS-less demux head, so the engine honestly
  // refuses). Mid-file keyframes carry DTS and cut normally.
  const QString src =
      makeTitledClip("Game_2026-01-15_20-30-00.mkv", "ClipGame", 5);
  if (src.isEmpty())
    GTEST_SKIP() << "ffmpeg CLI unavailable";
  RecordingLibrary lib(tempDir.path());
  waitForProbes();
  QSignalSpy saved(&lib, &RecordingLibrary::clipSaved);
  QSignalSpy failed(&lib, &RecordingLibrary::clipSaveFailed);
  lib.saveClipRange(src, 2.0, 4.0);
  EXPECT_TRUE(lib.clipSaving());
  ASSERT_TRUE(saved.wait(15000)) << "clipSaved never arrived";
  EXPECT_EQ(failed.count(), 0);
  EXPECT_FALSE(lib.clipSaving());
  ASSERT_EQ(saved.count(), 1);
  const QList<QVariant> args = saved.first();
  const QString out = args.first().toString();
  EXPECT_TRUE(QFile::exists(out));
  EXPECT_TRUE(out.endsWith("_Replay.mkv"));
  // Filed under the source's game, with the span the file holds.
  EXPECT_EQ(args.at(1).toString(), QString("ClipGame"));
  const double secs = args.at(2).toDouble();
  EXPECT_GE(secs, 2.0);
  EXPECT_LE(secs, 3.0);
}

TEST_F(RecordingLibraryTest, SaveClipRangeUsesUniqueNameOnCollision) {
  const QString src =
      makeTitledClip("Game_2026-01-15_20-30-00.mkv", "ClipGame", 5);
  if (src.isEmpty())
    GTEST_SKIP() << "ffmpeg CLI unavailable";
  RecordingLibrary lib(tempDir.path());
  waitForProbes();

  // Pre-create the same-second destination that the save operation will
  // choose. Also cover an immediate second rollover while the test starts.
  const QDateTime now = QDateTime::currentDateTime();
  QStringList occupied;
  for (int delta = -1; delta <= 2; ++delta) {
    const QString ts = now.addSecs(delta).toString("yyyy-MM-dd_HH-mm-ss");
    const QString path = tempDir.path() + "/ClipGame_" + ts + "_Replay.mkv";
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("occupied"), 8);
    occupied.append(path);
  }

  QSignalSpy saved(&lib, &RecordingLibrary::clipSaved);
  QSignalSpy failed(&lib, &RecordingLibrary::clipSaveFailed);
  lib.saveClipRange(src, 2.0, 4.0);
  ASSERT_TRUE(saved.wait(15000)) << "clipSaved never arrived";
  EXPECT_EQ(failed.count(), 0);
  ASSERT_EQ(saved.count(), 1);
  const QString output = saved.first().first().toString();
  EXPECT_FALSE(occupied.contains(output));
  EXPECT_TRUE(QFile::exists(output));
  for (const QString &path : occupied) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("occupied"));
  }
}

TEST_F(RecordingLibraryTest, AppWiredRenamePreservesThumbnail) {
  // Full in-app rename flow: StorageManager moves the video + sidecar
  // thumbnail, recordingRenamed is wired to the library exactly like
  // Application wires it (onRecordingRenamed BEFORE refresh), plus the
  // explicit refresh main.qml's performRename issues. The recreated
  // Recording must carry the moved thumbnail — a blank card that only
  // recovers after restart means this regressed.
  const QString oldPath = createFile("Old_Game_2026-01-15_20-30-00.mkv");
  QDir(tempDir.path()).mkpath(".thumbnails");
  const QString oldThumb =
      tempDir.path() + "/.thumbnails/Old_Game_2026-01-15_20-30-00.jpg";
  {
    QFile thumb(oldThumb);
    ASSERT_TRUE(thumb.open(QIODevice::WriteOnly));
    thumb.write(QByteArray(512, 't'));
  }

  StorageManager storage(tempDir.path());
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);
  Recording *before = lib.getRecording(oldPath);
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(before->thumbnailPath(), oldThumb);

  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::onRecordingRenamed);
  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::refresh);

  QSignalSpy changedSpy(&lib, &RecordingLibrary::recordingsChanged);
  QSignalSpy fileChangedSpy(before, &Recording::fileChanged);
  const QString newPath = storage.renameRecording(oldPath, "New Title");
  ASSERT_FALSE(newPath.isEmpty());
  // The rename's signal-driven refresh must notify (the grid only
  // rebinds on recordingsChanged; correct content without the emit
  // leaves the card bound to the deleted pre-rename object: blank
  // until restart).
  EXPECT_GE(changedSpy.count(), 1) << "rename produced no model notification";
  EXPECT_EQ(lib.count(), 1);
  Recording *signaled = lib.getRecording(newPath);
  // Adopted in place: the same object survives, so delegates never
  // observe a deleted recording even if a view rebuild lags.
  EXPECT_EQ(signaled, before) << "rename swapped the Recording object";
  ASSERT_NE(signaled, nullptr);
  EXPECT_FALSE(signaled->thumbnailPath().isEmpty());
  EXPECT_GE(fileChangedSpy.count(), 1) << "rename did not notify fileChanged";
  EXPECT_EQ(signaled->filePath(), newPath);
  EXPECT_EQ(signaled->fileName(), QFileInfo(newPath).fileName());
  EXPECT_EQ(signaled->gameName(), QString("New Title"));
  // main.qml performRename() issues one more refresh after the rename.
  lib.refresh();
  waitForProbes();

  ASSERT_EQ(lib.count(), 1);
  Recording *after = lib.getRecording(newPath);
  ASSERT_NE(after, nullptr) << "renamed file missing from library";
  EXPECT_EQ(after, before);
  EXPECT_FALSE(after->thumbnailPath().isEmpty())
      << "thumbnail lost across rename (card goes blank until restart)";
  if (!after->thumbnailPath().isEmpty())
    EXPECT_TRUE(QFile::exists(after->thumbnailPath()));
}

TEST_F(RecordingLibraryTest, RenameDuringProbeKeepsNewName) {
  // The initial probe of a real file takes tens of ms; renaming
  // immediately (as the app flow does) must not let the stale
  // in-flight completion overwrite the adopted name afterwards.
  const QString oldPath =
      makeTitledClip("Old_Game_2026-01-15_20-30-00.mkv", "Old Game");
  if (oldPath.isEmpty())
    GTEST_SKIP() << "ffmpeg CLI unavailable";

  StorageManager storage(tempDir.path());
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 1);
  Recording *rec = lib.getRecording(oldPath);
  ASSERT_NE(rec, nullptr);

  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::onRecordingRenamed);
  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::refresh);

  const QString newPath = storage.renameRecording(oldPath, "New Title");
  ASSERT_FALSE(newPath.isEmpty());
  lib.refresh();
  waitForProbes();

  ASSERT_EQ(lib.count(), 1);
  Recording *after = lib.getRecording(newPath);
  ASSERT_NE(after, nullptr);
  EXPECT_EQ(after, rec) << "rename during probe swapped the object";
  EXPECT_EQ(after->gameName(), QString("New Title"))
      << "stale probe completion reverted the adopted name";
  EXPECT_GT(after->duration(), 0) << "adopted probe never filled metadata";
}

TEST_F(RecordingLibraryTest, RenamePreservesClipIdentityWhenSuffixStripped) {
  // Legacy clips carry _Replay in the name but no trophy-kind tag;
  // renaming drops the timestamp/_Replay suffix, which must not flip
  // the Clip badge to Recording.
  const QString oldPath =
      makeTitledClip("ClipGame_2026-01-15_20-30-00_Replay.mkv", "ClipGame");
  if (oldPath.isEmpty())
    GTEST_SKIP() << "ffmpeg CLI unavailable";

  StorageManager storage(tempDir.path());
  RecordingLibrary lib(tempDir.path());
  waitForProbes();
  ASSERT_EQ(lib.count(), 1);
  ASSERT_TRUE(lib.get(0)->isClip());

  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::onRecordingRenamed);
  QObject::connect(&storage, &StorageManager::recordingRenamed, &lib,
                   &RecordingLibrary::refresh);

  const QString newPath = storage.renameRecording(oldPath, "Cool");
  ASSERT_FALSE(newPath.isEmpty());
  // No timestamp to preserve (old name ended in _Replay): plain name.
  EXPECT_EQ(newPath, tempDir.path() + "/Cool.mkv");
  lib.refresh();
  waitForProbes();

  ASSERT_EQ(lib.count(), 1);
  Recording *after = lib.getRecording(newPath);
  ASSERT_NE(after, nullptr);
  EXPECT_TRUE(after->isClip()) << "rename flipped clip identity";
  EXPECT_EQ(after->gameName(), QString("Cool"));
}

TEST_F(RecordingLibraryTest, RenameAdoptionFallsBackWhenSourceUntracked) {
  // A rename notification for a source the library never tracked
  // cannot adopt in place: the file still arrives via the classic
  // remove + add scan path.
  const QString tracked = createFile("Tracked_2026-01-15_20-30-00.mkv");
  const QString dropped = createFile("Fresh_2026-01-15_20-30-00.mkv");
  RecordingLibrary lib(tempDir.path());
  ASSERT_EQ(lib.count(), 2);

  QSignalSpy removedSpy(&lib, &RecordingLibrary::recordingRemoved);
  lib.onRecordingRenamed(tempDir.path() + "/Ghost_2026-01-15_20-30-00.mkv",
                         dropped);
  lib.refresh();

  EXPECT_EQ(lib.count(), 2);
  EXPECT_NE(lib.getRecording(tracked), nullptr);
  EXPECT_NE(lib.getRecording(dropped), nullptr);
  EXPECT_EQ(removedSpy.count(), 0);
}
