/*
 * Trophy - Unit Tests
 * Test suite for Recording metadata parsing
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QThreadPool>
#include <gtest/gtest.h>

#include "library/Recording.h"
#include "muxing/TrophyMetadata.h"

using namespace TrophyBackend;

class RecordingTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new RecordingTestEnvironment);

class RecordingTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  void TearDown() override {
    // loadMetadata() probes on the global thread pool; make sure no probe
    // outlives the stack-allocated Recording under test.
    QThreadPool::globalInstance()->waitForDone(5000);
  }

  // Create a dummy file and return its full path
  QString createFile(const QString &name, qint64 size = 8192) {
    QString path = tempDir.path() + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
      return QString();
    }
    f.write(QByteArray(size, '\0'));
    f.close();
    return path;
  }

  // Render a real 1s clip with embedded container tags using the
  // system ffmpeg. Empty string when ffmpeg is unavailable (callers
  // should SKIP).
  QString makeTitledClip(const QString &name, const QString &title,
                         const QString &kind = QString(),
                         const QMap<QString, QString> &extra = {}) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QStringList args{"-y",
                     "-v",
                     "error",
                     "-f",
                     "lavfi",
                     "-i",
                     "testsrc=size=320x240:rate=30:duration=1",
                     "-pix_fmt",
                     "yuv420p",
                     "-metadata",
                     QString("title=%1").arg(title)};
    if (!kind.isEmpty())
      args << "-metadata" << QString("trophy-kind=%1").arg(kind);
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
      args << "-metadata" << QString("%1=%2").arg(it.key(), it.value());
    args << path;
    QProcess proc;
    proc.start("ffmpeg", args);
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }

  void waitForProbe() {
    QThreadPool::globalInstance()->waitForDone(10000);
    QCoreApplication::processEvents();
  }
};

// --- Constructor metadata ---

TEST_F(RecordingTest, ConstructorSetsFilePath) {
  QString path = createFile("Test_Game_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_EQ(rec.filePath(), path);
}

TEST_F(RecordingTest, ConstructorSetsFileName) {
  QString path = createFile("Test_Game_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_EQ(rec.fileName(), "Test_Game_2026-01-15_20-30-00.mkv");
}

TEST_F(RecordingTest, ConstructorSetsDirectory) {
  QString path = createFile("Test_Game_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_EQ(rec.directory(), tempDir.path());
}

TEST_F(RecordingTest, ConstructorSetsSize) {
  QString path = createFile("Test_Game_2026-01-15_20-30-00.mkv", 5000);
  Recording rec(path);
  EXPECT_EQ(rec.size(), 5000);
}

TEST_F(RecordingTest, ConstructorSetsDate) {
  QString path = createFile("Test_Game_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_TRUE(rec.date().isValid());
}

// --- parseFileNameForGameName() (via loadMetadata) ---

TEST_F(RecordingTest, ParseStandardFilename) {
  QString path = createFile("Cyberpunk_2077_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Cyberpunk 2077");
}

TEST_F(RecordingTest, ParseSpacesInName) {
  QString path = createFile("The_Witcher_3_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "The Witcher 3");
}

TEST_F(RecordingTest, ParseGenericRecording) {
  QString path = createFile("Recording_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Unknown Game");
}

TEST_F(RecordingTest, ParseNoTimestamp) {
  QString path = createFile("random_video.mkv");
  Recording rec(path);
  rec.loadMetadata();
  // Without a timestamp pattern, uses the full basename
  EXPECT_EQ(rec.gameName(), "random_video");
}

TEST_F(RecordingTest, ParseSingleWordGame) {
  QString path = createFile("Hades_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Hades");
}

TEST_F(RecordingTest, ParseMultiUnderscoreGame) {
  QString path = createFile("Path_Of_Exile_2_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Path Of Exile 2");
}

TEST_F(RecordingTest, ParseMP4Extension) {
  QString path = createFile("Factorio_2026-12-25_23-59-59.mp4");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Factorio");
}

TEST_F(RecordingTest, ParseClipFilename) {
  QString path = createFile("Deadlock_2026-10-01_19-31-31_Replay.mkv");
  Recording rec(path);
  rec.loadMetadata();
  EXPECT_EQ(rec.gameName(), "Deadlock");
}

// --- trophy-kind container tag (survives renames, unlike the suffix) ---

TEST_F(RecordingTest, KindTagMarksClipWithoutSuffix) {
  QString path = makeTitledClip("Custom_Name.mkv", "Deadlock", "clip");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  EXPECT_FALSE(rec.isClip());
  rec.loadMetadata();
  waitForProbe();
  EXPECT_TRUE(rec.isClip());
}

TEST_F(RecordingTest, NoKindTagLeavesRecording) {
  QString path = makeTitledClip("Deadlock_2026-10-01_19-31-31.mkv", "Deadlock");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_FALSE(rec.isClip());
  EXPECT_EQ(rec.gameName(), "Deadlock");
}

// --- trophy-version container tag ---

TEST_F(RecordingTest, TrophyVersionTagIsExposed) {
  QMap<QString, QString> extra{{"trophy-version", "9.9.9-test"}};
  QString path =
      makeTitledClip("Versioned.mkv", "Deadlock", "recording", extra);
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  EXPECT_TRUE(rec.trophyVersion().isEmpty());
  rec.loadMetadata();
  waitForProbe();
  EXPECT_EQ(rec.trophyVersion(), "9.9.9-test");
}

TEST_F(RecordingTest, NoVersionTagLeavesVersionEmpty) {
  QString path = makeTitledClip("Deadlock_2026-10-01_19-31-31.mkv", "Deadlock");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_TRUE(rec.trophyVersion().isEmpty());
}

// --- trophy-date + trophy-time container tags (replace filename ts) ---

TEST_F(RecordingTest, TrophyDateTimeTagsSetDate) {
  // creation_time deliberately disagrees: the explicit pair must win.
  QMap<QString, QString> extra{{"creation_time", "2021-05-06T07:08:09"},
                               {"trophy-date", "2020-01-02"},
                               {"trophy-time", "03-04-05"}};
  QString path =
      makeTitledClip("Custom_Name.mkv", "Deadlock", "recording", extra);
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_EQ(rec.date().toString("yyyy-MM-dd_HH-mm-ss"),
            QString("2020-01-02_03-04-05"));
}

TEST_F(RecordingTest, MalformedTrophyDateFallsBackToCreationTime) {
  // Explicit Z: the matroska muxer normalizes TZ-less creation_time to
  // UTC on write (one more reason trophy-date/time exist), so pin the
  // offset to keep this TZ-independent.
  QMap<QString, QString> extra{{"creation_time", "2021-05-06T07:08:09Z"},
                               {"trophy-date", "not-a-date"},
                               {"trophy-time", "03-04-05"}};
  QString path =
      makeTitledClip("Custom_Name.mkv", "Deadlock", "recording", extra);
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_EQ(rec.date().toString("yyyy-MM-dd_HH-mm-ss"),
            QString("2021-05-06_07-08-09"));
}

TEST_F(RecordingTest, HalfTrophyDateTimeFallsBackToCreationTime) {
  // Date without time is not a capture moment: ignore the half pair.
  QMap<QString, QString> extra{{"creation_time", "2021-05-06T07:08:09Z"},
                               {"trophy-date", "2020-01-02"}};
  QString path =
      makeTitledClip("Custom_Name.mkv", "Deadlock", "recording", extra);
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_EQ(rec.date().toString("yyyy-MM-dd_HH-mm-ss"),
            QString("2021-05-06_07-08-09"));
}

// --- TrophyMetadata helpers (no ffmpeg needed) ---

TEST(TrophyMetadataHelpers, DateTimeRoundTrip) {
  const QDateTime now = QDateTime::currentDateTime();
  const QString stamp = now.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
  const QDateTime parsed =
      trophyDateTimeFromTags(trophyDateString(now), trophyTimeString(now));
  EXPECT_TRUE(parsed.isValid());
  EXPECT_EQ(parsed.toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")), stamp);
  // Recorder stamp is second-granular by construction.
  EXPECT_EQ(parsed.time().msec(), 0);
}

TEST(TrophyMetadataHelpers, DateTimeRejectsGarbage) {
  EXPECT_FALSE(trophyDateTimeFromTags(QStringLiteral("not-a-date"),
                                      QStringLiteral("03-04-05"))
                   .isValid());
  EXPECT_FALSE(
      trophyDateTimeFromTags(QStringLiteral("2020-01-02"), QStringLiteral(""))
          .isValid());
  EXPECT_FALSE(
      trophyDateTimeFromTags(QStringLiteral(""), QStringLiteral("03-04-05"))
          .isValid());
}

TEST(TrophyMetadataHelpers, VersionIsNeverEmpty) {
  EXPECT_FALSE(trophyVersion().isEmpty());
}

// --- Thumbnail path ---

TEST_F(RecordingTest, ThumbnailPathUpdateEmitsSignal) {
  QString path = createFile("Test_2026-01-15_20-30-00.mkv");
  Recording rec(path);

  QSignalSpy spy(&rec, &Recording::thumbnailPathChanged);
  rec.setThumbnailPath("/tmp/thumb.jpg");

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(rec.thumbnailPath(), "/tmp/thumb.jpg");
}

TEST_F(RecordingTest, ThumbnailPathNoSignalOnSame) {
  QString path = createFile("Test_2026-01-15_20-30-00.mkv");
  Recording rec(path);

  rec.setThumbnailPath("/tmp/thumb.jpg");

  QSignalSpy spy(&rec, &Recording::thumbnailPathChanged);
  rec.setThumbnailPath("/tmp/thumb.jpg"); // Same value
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(RecordingTest, ThumbnailAutoDetection) {
  // Create the thumbnails subdirectory and a matching thumbnail
  QDir(tempDir.path()).mkdir(".thumbnails");
  QString thumbPath =
      tempDir.path() + "/.thumbnails/TestGame_2026-01-15_20-30-00.jpg";
  QFile tf(thumbPath);
  if (!tf.open(QIODevice::WriteOnly)) {
    return;
  }
  tf.write("fake thumbnail data");
  tf.close();

  // Create the video file
  QString vidPath = createFile("TestGame_2026-01-15_20-30-00.mkv");
  Recording rec(vidPath);

  // Constructor should auto-detect the thumbnail
  EXPECT_EQ(rec.thumbnailPath(), thumbPath);
}

TEST_F(RecordingTest, NoThumbnailByDefault) {
  QString path = createFile("NothumbGame_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_TRUE(rec.thumbnailPath().isEmpty());
}

// --- Initial dimension/duration values ---

TEST_F(RecordingTest, DefaultDimensionsZero) {
  QString path = createFile("Test_2026-01-15_20-30-00.mkv");
  Recording rec(path);
  EXPECT_EQ(rec.width(), 0);
  EXPECT_EQ(rec.height(), 0);
  EXPECT_EQ(rec.duration(), 0);
}

// --- Container title vs filename precedence ---

TEST_F(RecordingTest, ContainerTitleOverridesFileNameByDefault) {
  const QString path =
      makeTitledClip("Placeholder_2026-01-15_20-30-00.mkv", "Embedded Title");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  Recording rec(path);
  rec.loadMetadata();
  waitForProbe();
  // Trophy bakes the game name into the container at record time, and the
  // probe prefers that tag over the filename.
  EXPECT_EQ(rec.gameName(), "Embedded Title");
}

TEST_F(RecordingTest, ProbePublishesMetadataOnOwningThread) {
  const QString path = makeTitledClip("Filename_Title_2026-01-15_20-30-00.mkv",
                                      "Container Title");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  Recording rec(path);
  const QThread *ownerThread = QThread::currentThread();
  bool signalOnOwnerThread = false;
  QObject::connect(&rec, &Recording::metadataChanged, &rec, [&]() {
    signalOnOwnerThread = QThread::currentThread() == ownerThread;
  });
  rec.loadMetadata();
  QThreadPool::globalInstance()->waitForDone(10000);
  // Worker completion queues the result; it must not mutate public QObject
  // state before the owning thread dispatches that queued publication.
  EXPECT_EQ(rec.gameName(), "Filename Title");
  QCoreApplication::processEvents();
  EXPECT_EQ(rec.gameName(), "Container Title");
  EXPECT_TRUE(signalOnOwnerThread);
}

TEST_F(RecordingTest, PreferFileNameTitleKeepsRenamedName) {
  const QString oldPath =
      makeTitledClip("Old_Game_2026-01-15_20-30-00.mkv", "Old Game");
  if (oldPath.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or clip render failed";

  // Sanity: without the flag the stale embedded title wins.
  {
    Recording before(oldPath);
    before.loadMetadata();
    waitForProbe();
    ASSERT_EQ(before.gameName(), "Old Game");
  }

  // Simulate an in-app rename: new filename, same bytes (stale title tag).
  const QString newPath = tempDir.path() + "/New_Title_2026-01-15_20-30-00.mkv";
  ASSERT_TRUE(QFile::rename(oldPath, newPath));

  Recording rec(newPath);
  rec.setPreferFileNameTitle(true);
  rec.loadMetadata();
  waitForProbe();
  EXPECT_EQ(rec.gameName(), "New Title");
  // Duration/resolution probing still works with the flag set.
  EXPECT_EQ(rec.width(), 320);
  EXPECT_EQ(rec.height(), 240);
}
