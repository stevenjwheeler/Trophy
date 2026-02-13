/*
 * Trophy - Unit Tests
 * Test suite for Recording metadata parsing
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "library/Recording.h"

using namespace GameRecorder;

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

  // Create a dummy file and return its full path
  QString createFile(const QString &name, qint64 size = 8192) {
    QString path = tempDir.path() + "/" + name;
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(QByteArray(size, '\0'));
    f.close();
    return path;
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
  tf.open(QIODevice::WriteOnly);
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
