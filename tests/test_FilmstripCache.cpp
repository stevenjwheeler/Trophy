/*
 * Trophy - Unit Tests
 * FilmstripCache: evenly-spaced thumbnail extraction, disk
 * reuse and failure signalling.
 */

#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QImage>
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

TEST_F(FilmstripCacheTest, DifferentCountsDoNotReuseStaleStrip) {
  const QString path = renderClip("counts.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  FilmstripCache cache;
  QSignalSpy first(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 2);
  ASSERT_TRUE(first.wait(10000));
  const QStringList small = first.first().at(1).toStringList();
  ASSERT_EQ(small.size(), 2);

  QSignalSpy second(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 3);
  ASSERT_TRUE(second.wait(10000));
  const QStringList large = second.first().at(1).toStringList();
  ASSERT_EQ(large.size(), 3);
  EXPECT_NE(large.first(), small.first());
  for (const QString &thumb : large)
    EXPECT_TRUE(QFile::exists(thumb));
}

TEST_F(FilmstripCacheTest, FreshWriteSweepsOnlyLegacyCountlessStrips) {
  const QString path = renderClip("legacy.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  const QString dir = tempDir.path() + "/.thumbnails";
  ASSERT_TRUE(QDir().mkpath(dir));
  const QString legacy = dir + "/legacy.strip-0.jpg";
  const QString unrelated = dir + "/other.strip-0.jpg";
  for (const QString &name : {legacy, unrelated}) {
    QFile f(name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write("old"), 3);
  }

  FilmstripCache cache;
  QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 2);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.first().at(1).toStringList().size(), 2);
  EXPECT_FALSE(QFile::exists(legacy));
  EXPECT_TRUE(QFile::exists(unrelated));
}

TEST_F(FilmstripCacheTest, SingleDelayedFrameDrainsAtEof) {
  if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable";
  const QString path = tempDir.path() + "/one-frame.mkv";
  QProcess render;
  render.start("ffmpeg", {"-y", "-v", "error", "-f", "lavfi", "-i",
                         "color=c=red:size=64x48:rate=30", "-frames:v", "1",
                         "-c:v", "libx264", "-pix_fmt", "yuv420p", path});
  ASSERT_TRUE(render.waitForFinished(10000));
  if (render.exitCode() != 0)
    GTEST_SKIP() << "libx264 CLI unavailable: "
                 << render.readAllStandardError().constData();
  FilmstripCache cache;
  QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
  cache.requestThumbs(path, 1);
  ASSERT_TRUE(spy.wait(10000));
  const QStringList thumbs = spy.first()[1].toStringList();
  ASSERT_EQ(thumbs.size(), 1);
  const QImage image(thumbs.first());
  ASSERT_FALSE(image.isNull());
  EXPECT_EQ(image.size(), QSize(160, 120));
  EXPECT_GT(image.pixelColor(80, 60).red(), 200);
}

TEST_F(FilmstripCacheTest, NativeSidecarWriteFailureIsNotACacheHitAndRetries) {
  const QString path = renderClip("write-failure.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  const QString blocked = tempDir.path() +
      "/.thumbnails/write-failure.strip-2-1.jpg";
  // A directory at a JPEG destination forces an actual filesystem open
  // failure, including as root. No permissions/ambient files are changed.
  ASSERT_TRUE(QDir().mkpath(blocked));
  FilmstripCache cache;
  QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
  for (int attempt = 0; attempt < 2; ++attempt) {
    cache.requestThumbs(path, 2);
    ASSERT_TRUE(spy.wait(10000));
    ASSERT_EQ(spy.size(), 1);
    EXPECT_TRUE(spy.first()[1].toStringList().isEmpty());
    spy.clear();
  }
  ASSERT_TRUE(QDir().rmdir(blocked));
  cache.requestThumbs(path, 2);
  ASSERT_TRUE(spy.wait(10000));
  const QStringList thumbs = spy.first()[1].toStringList();
  ASSERT_EQ(thumbs.size(), 2);
  for (const QString &thumb : thumbs)
    EXPECT_FALSE(QImage(thumb).isNull());
}

TEST_F(FilmstripCacheTest, BoundedTruncatedMediaCorpusSettlesAndRecovers) {
  const QString source = renderClip("corpus-source.mkv");
  if (source.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";
  QFile input(source);
  ASSERT_TRUE(input.open(QIODevice::ReadOnly));
  const QByteArray valid = input.readAll();
  ASSERT_GT(valid.size(), 512);
  const QList<qsizetype> sizes = {0, 1, 16, 64, 256, 512,
                                 valid.size() / 2, valid.size() - 1};
  FilmstripCache cache;
  for (qsizetype size : sizes) {
    SCOPED_TRACE(size);
    const QString path = tempDir.path() + QString("/truncated-%1.mkv").arg(size);
    QFile media(path);
    ASSERT_TRUE(media.open(QIODevice::WriteOnly));
    ASSERT_EQ(media.write(valid.constData(), size), size);
    media.close();
    QSignalSpy spy(&cache, &FilmstripCache::filmstripReady);
    cache.requestThumbs(path, 2);
    ASSERT_TRUE(spy.wait(10000));
    ASSERT_EQ(spy.size(), 1);
    const QStringList partial = spy.first()[1].toStringList();
    EXPECT_TRUE(partial.isEmpty() || partial.size() == 2);
    for (const QString &thumb : partial)
      EXPECT_FALSE(QImage(thumb).isNull());
    // Remove only this fixture's sidecars before retrying the same path.
    QDir thumbs(tempDir.path() + "/.thumbnails");
    for (const QString &name : thumbs.entryList(
             {QFileInfo(path).completeBaseName() + ".strip-*.jpg"}, QDir::Files))
      ASSERT_TRUE(thumbs.remove(name));
    ASSERT_TRUE(media.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(media.write(valid), valid.size());
    media.close();
    spy.clear();
    cache.requestThumbs(path, 2);
    ASSERT_TRUE(spy.wait(10000));
    ASSERT_EQ(spy.size(), 1);
    ASSERT_EQ(spy.first()[1].toStringList().size(), 2);
  }
}
