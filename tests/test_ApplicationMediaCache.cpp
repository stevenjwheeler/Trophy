/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise post-save cache preparation without initializing app services.
 */
#include "core/Application.h"
#include "library/FilmstripCache.h"
#include "library/WaveformCache.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <gtest/gtest.h>

namespace TrophyBackend {
namespace {
// Keep native cache work queued until every foreground request has joined.
class WorkerGate {
public:
  WorkerGate() : pool(QThreadPool::globalInstance()), maximum(pool->maxThreadCount()) {
    pool->setMaxThreadCount(1);
    pool->start([this]() {
      entered.release();
      proceed.acquire();
    });
    entered.acquire();
  }
  void release() {
    if (!released) {
      released = true;
      proceed.release();
    }
  }
  ~WorkerGate() {
    release();
    pool->waitForDone();
    pool->setMaxThreadCount(maximum);
  }
private:
  QThreadPool *pool;
  int maximum;
  QSemaphore entered;
  QSemaphore proceed;
  bool released = false;
};
} // namespace

class ApplicationMediaCacheTest : public ::testing::Test {
protected:
  Application &app = *static_cast<Application *>(QApplication::instance());
  QTemporaryDir tempDir;

  void SetUp() override {
    ASSERT_TRUE(tempDir.isValid());
    app.m_waveformCache = std::make_unique<WaveformCache>();
    app.m_filmstripCache = std::make_unique<FilmstripCache>();
  }
  void TearDown() override {
    QThreadPool::globalInstance()->waitForDone();
    QCoreApplication::processEvents();
    app.m_waveformCache.reset();
    app.m_filmstripCache.reset();
  }
  WaveformCache *waveforms() { return app.m_waveformCache.get(); }
  FilmstripCache *filmstrip() { return app.m_filmstripCache.get(); }
  void prepare(const QString &path) { app.preparePlayerCaches(path); }
  void settle() {
    ASSERT_TRUE(QThreadPool::globalInstance()->waitForDone(10000));
    QCoreApplication::processEvents();
  }
  QString render(bool audio = true) {
    if (QStandardPaths::findExecutable("ffmpeg").isEmpty())
      return {};
    const QString path = tempDir.filePath("Game Clip.mkv");
    QStringList args{"-y", "-v", "error", "-f", "lavfi", "-i",
                     "testsrc=size=160x90:rate=10:duration=1"};
    if (audio)
      args << "-f" << "lavfi" << "-i" << "sine=frequency=440:duration=1"
           << "-f" << "lavfi" << "-i" << "sine=frequency=880:duration=1"
           << "-map" << "0:v" << "-map" << "1:a" << "-map" << "2:a";
    args << "-c:v" << "mpeg4" << "-pix_fmt" << "yuv420p"
         << "-c:a" << "aac" << path;
    QProcess process;
    process.start("ffmpeg", args);
    if (!process.waitForFinished(30000) || process.exitCode() != 0)
      return {};
    return path;
  }
};

TEST_F(ApplicationMediaCacheTest, PreparesAllTracksAndPlayerJoinsPendingJobs) {
  const QString path = render();
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg fixture unavailable";
  QSignalSpy peaks(waveforms(), &WaveformCache::peaksReady);
  QSignalSpy strip(filmstrip(), &FilmstripCache::filmstripReady);
  WorkerGate gate;
  prepare(path);
  prepare(path); // Duplicate completion must not start another decode.
  waveforms()->requestPeaks(path, 1); // Player opens before delivery.
  filmstrip()->requestThumbs(path, 32);
  EXPECT_TRUE(peaks.isEmpty());
  EXPECT_TRUE(strip.isEmpty());
  gate.release();
  settle();
  ASSERT_EQ(peaks.count(), 2);
  for (const auto &result : peaks)
    EXPECT_EQ(result.at(2).toList().size(), WaveformCache::kBuckets);
  ASSERT_EQ(strip.count(), 1);
  EXPECT_EQ(strip.first().at(1).toStringList().size(), 32);
  EXPECT_TRUE(QFileInfo::exists(tempDir.filePath(".thumbnails/Game Clip.peaks")));
  for (const QString &thumb : strip.first().at(1).toStringList())
    EXPECT_GT(QFileInfo(thumb).size(), 0);

  // A later open uses the disk caches without rewriting them.
  const QFileInfo cache(tempDir.filePath(".thumbnails/Game Clip.peaks"));
  const auto stamp = cache.lastModified();
  peaks.clear();
  strip.clear();
  waveforms()->requestPeaks(path, 1);
  filmstrip()->requestThumbs(path, 32);
  settle();
  ASSERT_EQ(peaks.count(), 1);
  EXPECT_EQ(peaks.first().at(2).toList().size(), WaveformCache::kBuckets);
  ASSERT_EQ(strip.count(), 1);
  EXPECT_EQ(QFileInfo(cache.filePath()).lastModified(), stamp);
}

TEST_F(ApplicationMediaCacheTest, VideoOnlyStillPreparesFilmstrip) {
  const QString path = render(false);
  if (path.isEmpty())
    GTEST_SKIP() << "ffmpeg fixture unavailable";
  QSignalSpy peaks(waveforms(), &WaveformCache::peaksReady);
  QSignalSpy strip(filmstrip(), &FilmstripCache::filmstripReady);
  prepare(path);
  settle();
  ASSERT_EQ(peaks.count(), 1);
  EXPECT_TRUE(peaks.first().at(2).toList().isEmpty());
  ASSERT_EQ(strip.count(), 1);
  EXPECT_EQ(strip.first().at(1).toStringList().size(), 32);
}

TEST_F(ApplicationMediaCacheTest, MissingOrEmptyCompletionDoesNotStartJobs) {
  QSignalSpy peaks(waveforms(), &WaveformCache::peaksReady);
  QSignalSpy strip(filmstrip(), &FilmstripCache::filmstripReady);
  prepare({});
  prepare(tempDir.filePath("deleted.mkv"));
  prepare(tempDir.path());
  settle();
  EXPECT_TRUE(peaks.isEmpty());
  EXPECT_TRUE(strip.isEmpty());
  EXPECT_FALSE(QDir(tempDir.filePath(".thumbnails")).exists());
}
} // namespace TrophyBackend
