/*
 * Trophy - Unit Tests
 * ReplayManager: segment window selection, pruning, and save guards.
 * No portal/pipeline needed (pure logic + inactive-state signals).
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "core/ReplayManager.h"

using namespace TrophyBackend;

class ReplayManagerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new ReplayManagerTestEnvironment);

using Seg = ReplayManager::Segment;

namespace TrophyBackend {

class ReplayManagerTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_TRUE(cacheDir.isValid()); }

  // Creates a non-empty file so existence checks are meaningful.
  QString makeSegFile(const QString &name) {
    const QString path = cacheDir.path() + "/" + name;
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("fake-segment");
    f.close();
    return path;
  }

  QTemporaryDir cacheDir;

  // Friendship is not inherited by TEST_F bodies, so private access
  // lives in these fixture helpers (same pattern as the detection tests).
  void fabricateMidRecordingState(ReplayManager &mgr, const QString &closed,
                                  const QString &open) {
    mgr.m_segments = {{closed, 0.0, 60.0}};
    mgr.m_openSegmentPath = open;
    mgr.m_openSegmentStart = 60.0;
  }
  bool hasBufferedState(const ReplayManager &mgr) const {
    return !mgr.m_segments.isEmpty() || !mgr.m_openSegmentPath.isEmpty();
  }
  // Friendship is not inherited by TEST_F bodies, so private access
  // lives here (same pattern as the fabrication helpers above).
  QString defaultCacheDir(ReplayManager &mgr) { return mgr.cacheDir(); }
  QString legacyCacheDir() { return ReplayManager::legacyCacheDir(); }
};

} // namespace TrophyBackend

using TrophyBackend::ReplayManagerTest;

static QList<Seg> threeSegments() {
  // 60s segments starting at 0/60/120.
  return {{"s0.mkv", 0.0, 60.0}, {"s1.mkv", 60.0, 60.0}, {"s2.mkv", 120.0, 60.0}};
}

TEST_F(ReplayManagerTest, SelectForWindowKeepsCoveringSegments) {
  auto segs = threeSegments();
  // now=180, window=30: only the tail segment covers it.
  auto sel = ReplayManager::selectForWindow(segs, 180.0, 30.0);
  ASSERT_EQ(sel.size(), 1);
  EXPECT_EQ(sel.front().path, "s2.mkv");
  EXPECT_DOUBLE_EQ(sel.front().startOffsetSec, 120.0);

  // Window spanning the boundary keeps both, oldest first.
  sel = ReplayManager::selectForWindow(segs, 180.0, 70.0);
  ASSERT_EQ(sel.size(), 2);
  EXPECT_EQ(sel[0].path, "s1.mkv");
  EXPECT_EQ(sel[1].path, "s2.mkv");

  // Empty input selects nothing.
  EXPECT_TRUE(ReplayManager::selectForWindow({}, 180.0, 30.0).isEmpty());
}

TEST_F(ReplayManagerTest, PruneListKeepsMarginAndMinimum) {
  auto segs = threeSegments();
  // now=180, window=30, margin=60: horizon=90. s0 ends at 60 < 90,
  // but 3 segments with min-keep 2 protects... s0 is prunable.
  auto dead = ReplayManager::pruneList(segs, 180.0, 30.0);
  ASSERT_EQ(dead.size(), 1);
  EXPECT_EQ(dead.front(), "s0.mkv");

  // Fresh buffer: nothing past the horizon.
  dead = ReplayManager::pruneList(segs, 100.0, 30.0);
  EXPECT_TRUE(dead.isEmpty());

  // Minimum retention: two segments are never pruned even when old.
  QList<Seg> two{{"a.mkv", 0.0, 60.0}, {"b.mkv", 60.0, 60.0}};
  dead = ReplayManager::pruneList(two, 1000.0, 30.0);
  EXPECT_TRUE(dead.isEmpty());
}

TEST_F(ReplayManagerTest, MigratesLegacyCacheDirOnce) {
  // Hermetic via a fake XDG_CACHE_HOME: fabricate the previous default
  // (<cache>/Trophy/trophy/replay) with a stale segment, then resolve the
  // default cache dir. The legacy dir must be gone afterwards.
  QTemporaryDir fakeCache;
  ASSERT_TRUE(fakeCache.isValid());
  const QByteArray oldCacheHome = qgetenv("XDG_CACHE_HOME");
  qputenv("XDG_CACHE_HOME", fakeCache.path().toUtf8());

  const QString legacy = legacyCacheDir();
  // Guard: never let this test touch the real cache.
  ASSERT_FALSE(legacy.isEmpty());
  ASSERT_TRUE(legacy.startsWith(fakeCache.path()));
  ASSERT_TRUE(QDir().mkpath(legacy));
  const QString stale = legacy + "/stale_000.mkv";
  {
    QFile f(stale);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("fake-segment");
  }

  ReplayManager mgr;
  EXPECT_EQ(defaultCacheDir(mgr), QDir::tempPath() + "/trophy/replay");
  EXPECT_FALSE(QFileInfo::exists(legacy));

  if (oldCacheHome.isNull())
    qunsetenv("XDG_CACHE_HOME");
  else
    qputenv("XDG_CACHE_HOME", oldCacheHome);
}

TEST_F(ReplayManagerTest, DefaultCacheDirIsRamBackedTemp) {
  // No settings: the fallback must be /tmp (QDir::tempPath()), not the
  // persistent system cache — DVR segments are transient by design.
  ReplayManager mgr;
  const QString dir = defaultCacheDir(mgr);
  EXPECT_EQ(dir, QDir::tempPath() + "/trophy/replay");
  EXPECT_TRUE(QDir(dir).exists());
  // Clean up after ourselves: the call mkpaths the dir.
  QDir(QDir::tempPath() + "/trophy").removeRecursively();
}

TEST_F(ReplayManagerTest, InactiveByDefaultAndSaveFailsCleanly) {
  ReplayManager mgr;
  EXPECT_FALSE(mgr.isActive());
  EXPECT_TRUE(mgr.currentGame().isEmpty());

  QSignalSpy failedSpy(&mgr, &ReplayManager::replayFailed);
  mgr.saveReplay();
  ASSERT_EQ(failedSpy.count(), 1);

  // Pausing an idle manager is a safe no-op.
  mgr.setPaused(true);
  mgr.setPaused(false);
  EXPECT_FALSE(mgr.isActive());
}

TEST_F(ReplayManagerTest, StopDvrClearsOpenAndClosedSegments) {
  ReplayManager mgr;
  // Fabricate a mid-recording state: one rotated (closed) segment plus
  // the currently open one. stopDvr must delete both — the open file
  // used to leak because only m_segments was cleaned.
  const QString closed = makeSegFile("seg_000.mkv");
  const QString open = makeSegFile("seg_001.mkv");
  fabricateMidRecordingState(mgr, closed, open);
  ASSERT_TRUE(hasBufferedState(mgr));

  mgr.stopDvr();

  EXPECT_FALSE(QFile::exists(closed));
  EXPECT_FALSE(QFile::exists(open));
  EXPECT_FALSE(hasBufferedState(mgr));
  EXPECT_FALSE(mgr.isActive());
}
