/*
 * Trophy - Unit Tests
 * FilmstripCache: evenly-spaced thumbnail extraction, disk
 * reuse and failure signalling.
 */

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>

#include "library/FilmstripCache.h"

using namespace TrophyBackend;

class FilmstripCacheTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new FilmstripCacheTestEnvironment);

class FilmstripCacheTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  void TearDown() override {
    // Frame grabs run on the global thread pool; join before
    // the temp dir (and its files) goes away.
    QThreadPool::globalInstance()->waitForDone(10000);
  }

  // Short clip with real video + audio. Empty when ffmpeg is
  // unavailable (callers should SKIP).
  QString renderClip(const QString &name) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    proc.start("ffmpeg", {"-y",
                          "-v",
                          "error",
                          "-f",
                          "lavfi",
                          "-i",
                          "testsrc=size=320x240:rate=10:duration=2",
                          "-f",
                          "lavfi",
                          "-i",
                          "sine=frequency=440:duration=2",
                          "-c:v",
                          "mpeg4",
                          "-pix_fmt",
                          "yuv420p",
                          "-c:a",
                          "aac",
                          path});
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }
};

TEST_F(FilmstripCacheTest, MissingFileGivesEmpty) {
  FilmstripCache cache;
  const QString ghost = tempDir.path() + "/ghost.mkv";

  // Still emits (with an empty list) so QML never waits forever.
  QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(ghost, 4);
  ASSERT_TRUE(spy.wait(5000));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first().at(0).toString(), ghost);
  EXPECT_TRUE(spy.first().at(1).toStringList().isEmpty());
}

TEST_F(FilmstripCacheTest, StripThumbsExistAndReused) {
  const QString path = renderClip("strip.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  FilmstripCache cache;
  QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 4);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.count(), 1);
  const QStringList thumbs = spy.first().at(1).toStringList();

  ASSERT_EQ(thumbs.size(), 4);
  for (const QString &thumb : thumbs) {
    EXPECT_TRUE(QFile::exists(thumb)) << thumb.toUtf8().constData();
    EXPECT_GT(QFileInfo(thumb).size(), 0);
  }

  // Second request serves the cached files (same paths back).
  QSignalSpy spy2(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 4);
  ASSERT_TRUE(spy2.wait(5000));
  ASSERT_EQ(spy2.count(), 1);
  EXPECT_EQ(spy2.first().at(1).toStringList(), thumbs);
}
