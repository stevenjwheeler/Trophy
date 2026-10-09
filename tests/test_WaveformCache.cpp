/*
 * Trophy - Unit Tests
 * WaveformCache: peak extraction, normalization, disk cache,
 * track counting and lane labels.
 */

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>

#include "library/WaveformCache.h"

using namespace TrophyBackend;

class WaveformCacheTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new WaveformCacheTestEnvironment);

class WaveformCacheTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;

  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }

  void TearDown() override {
    // Peak extraction runs on the global thread pool; join
    // before the temp dir (and its files) goes away.
    QThreadPool::globalInstance()->waitForDone(10000);
  }

  // Render with the system ffmpeg. Empty string when ffmpeg is
  // unavailable (callers should SKIP).
  QString render(const QString &name, const QStringList &args) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return QString();
    const QString path = tempDir.path() + "/" + name;
    QProcess proc;
    proc.start("ffmpeg", QStringList({"-y", "-v", "error"}) + args +
                             QStringList({path}));
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0 ||
        !QFile::exists(path))
      return QString();
    return path;
  }

  QString renderTone(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "sine=frequency=440:duration=1", "-c:a", "aac"});
  }

  QString renderSilence(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "anullsrc=channel_layout=mono:sample_rate=44100:d=1",
                         "-c:a", "aac"});
  }

  QString renderTwoTracks(const QString &name) {
    return render(name, {"-f", "lavfi", "-i",
                         "sine=frequency=440:duration=1", "-f", "lavfi",
                         "-i", "sine=frequency=880:duration=1", "-map",
                         "0:a", "-map", "1:a", "-c:a", "aac"});
  }

  static double listMax(const QVariantList &peaks) {
    double m = 0.0;
    for (const QVariant &v : peaks)
      m = std::max(m, v.toDouble());
    return m;
  }
};

TEST_F(WaveformCacheTest, TrackLabels) {
  EXPECT_EQ(WaveformCache::trackLabel(0, 1), "Audio");
  EXPECT_EQ(WaveformCache::trackLabel(0, 3), "Game");
  EXPECT_EQ(WaveformCache::trackLabel(1, 3), "Voice");
  EXPECT_EQ(WaveformCache::trackLabel(2, 3), "Mic");
  EXPECT_EQ(WaveformCache::trackLabel(0, 2), "Game");
  EXPECT_EQ(WaveformCache::trackLabel(1, 2), "Voice");
  EXPECT_EQ(WaveformCache::trackLabel(5, 6), "Track 6");
}

TEST_F(WaveformCacheTest, MissingFileGivesNothing) {
  WaveformCache cache;
  const QString ghost = tempDir.path() + "/ghost.mkv";
  EXPECT_EQ(cache.audioTrackCount(ghost), 0);

  // Still emits (with an empty list) so QML never waits forever.
  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(ghost, 0);
  ASSERT_TRUE(spy.wait(5000));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.first().at(0).toString(), ghost);
  EXPECT_TRUE(spy.first().at(2).toList().isEmpty());
}

TEST_F(WaveformCacheTest, TonePeaksLoudAndNormalized) {
  const QString path = renderTone("tone.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 1);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.count(), 1);
  const QVariantList peaks = spy.first().at(2).toList();

  ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
  for (const QVariant &v : peaks) {
    EXPECT_GE(v.toDouble(), 0.0);
    EXPECT_LE(v.toDouble(), 1.0);
  }
  // A full-scale sine normalizes near 1 somewhere.
  EXPECT_GT(listMax(peaks), 0.5);
}

TEST_F(WaveformCacheTest, SilencePeaksAllZero) {
  const QString path = renderSilence("silence.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 1);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy.wait(10000));
  ASSERT_EQ(spy.count(), 1);
  const QVariantList peaks = spy.first().at(2).toList();

  ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
  EXPECT_EQ(listMax(peaks), 0.0);
}

TEST_F(WaveformCacheTest, CacheWrittenAndReused) {
  const QString path = renderTone("cached.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  {
    QSignalSpy spy(&cache, &WaveformCache::peaksReady);
    cache.requestPeaks(path, 0);
    ASSERT_TRUE(spy.wait(10000));
  }
  EXPECT_TRUE(QFile::exists(tempDir.path() + "/.thumbnails/cached.peaks"));

  // Second request serves the sidecar (same values, no decode).
  QSignalSpy spy2(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  ASSERT_TRUE(spy2.wait(5000));
  ASSERT_EQ(spy2.count(), 1);
  EXPECT_EQ(spy2.first().at(2).toList().size(), WaveformCache::kBuckets);
  EXPECT_GT(listMax(spy2.first().at(2).toList()), 0.5);
}

TEST_F(WaveformCacheTest, TwoTracksPeaksSeparately) {
  const QString path = renderTwoTracks("stereo2.mkv");
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg unavailable or render failed";

  WaveformCache cache;
  ASSERT_EQ(cache.audioTrackCount(path), 2);

  QSignalSpy spy(&cache, &WaveformCache::peaksReady);
  cache.requestPeaks(path, 0);
  cache.requestPeaks(path, 1);
  // Both tracks are owed an emit; they may arrive together.
  QElapsedTimer timer;
  timer.start();
  while (spy.count() < 2 && timer.elapsed() < 10000)
    spy.wait(500);
  ASSERT_EQ(spy.count(), 2);
  for (const auto &args : spy) {
    const QVariantList peaks = args.at(2).toList();
    ASSERT_EQ(peaks.size(), WaveformCache::kBuckets);
    EXPECT_GT(listMax(peaks), 0.5);
  }
}
