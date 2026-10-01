/*
 * Trophy - Unit Tests
 * ActivityFeed: JSON persistence round-trip next to settings.json.
 */

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "library/ActivityFeed.h"

using namespace TrophyBackend;

class ActivityFeedTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new ActivityFeedTestEnvironment);

class ActivityFeedTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  QString feedPath() const { return tempDir.path() + "/activity.json"; }
  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }
};

TEST_F(ActivityFeedTest, StartsEmptyWithoutFile) {
  ActivityFeed feed(nullptr, feedPath());
  EXPECT_EQ(feed.count(), 0);
  EXPECT_TRUE(feed.events().isEmpty());
}

TEST_F(ActivityFeedTest, AddPersistsAndReloadsNewestFirst) {
  {
    ActivityFeed feed(nullptr, feedPath());
    feed.addEventAt(QDateTime::fromString("2026-09-30T18:00:00", Qt::ISODate),
                    "clip", "blue", "Clip saved: Deadlock", "Last 30 seconds");
    feed.addEventAt(QDateTime::fromString("2026-10-01T18:00:00", Qt::ISODate),
                    "deleted", "orange", "Deleted: Deadlock_Clip.mkv",
                    "Removed from library");
    EXPECT_EQ(feed.count(), 2);
    EXPECT_TRUE(QFile::exists(feedPath()));
  }
  ActivityFeed reloaded(nullptr, feedPath());
  ASSERT_EQ(reloaded.count(), 2);
  // Newest first: the deletion outranks the older clip save.
  EXPECT_EQ(reloaded.get(0)["title"].toString(),
            "Deleted: Deadlock_Clip.mkv");
  EXPECT_EQ(reloaded.get(0)["kind"].toString(), "deleted");
  EXPECT_EQ(reloaded.get(1)["title"].toString(), "Clip saved: Deadlock");
  EXPECT_EQ(reloaded.get(1)["detail"].toString(), "Last 30 seconds");
}

TEST_F(ActivityFeedTest, CorruptFileStartsEmpty) {
  QFile f(feedPath());
  ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
  f.write("{ not valid json");
  f.close();

  ActivityFeed feed(nullptr, feedPath());
  EXPECT_EQ(feed.count(), 0);
}

TEST_F(ActivityFeedTest, ClearEmptiesStore) {
  ActivityFeed feed(nullptr, feedPath());
  feed.addEvent("storage", "orange", "Storage Full", "Over limit");
  ASSERT_EQ(feed.count(), 1);
  feed.clear();
  EXPECT_EQ(feed.count(), 0);

  ActivityFeed reloaded(nullptr, feedPath());
  EXPECT_EQ(reloaded.count(), 0);
}

TEST_F(ActivityFeedTest, PathRoundTripsForClipLookup) {
  {
    ActivityFeed feed(nullptr, feedPath());
    feed.addEvent("clip", "blue", "Clip saved: Deadlock", "Last 30 seconds",
                  "/vids/Deadlock_2026-10-01_19-31-31_Replay.mkv");
    feed.addEvent("storage", "red", "Storage Full", "Over limit");
  }
  ActivityFeed reloaded(nullptr, feedPath());
  ASSERT_EQ(reloaded.count(), 2);
  // Newest first: the storage warning outranks the clip save.
  EXPECT_EQ(reloaded.get(0)["path"].toString(), "");
  EXPECT_EQ(reloaded.get(1)["path"].toString(),
            "/vids/Deadlock_2026-10-01_19-31-31_Replay.mkv");
}

TEST_F(ActivityFeedTest, LegacyEntryWithoutPathLoads) {
  QFile f(feedPath());
  ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
  f.write("{\"events\":[{\"at\":\"2026-10-01T19:24:32.432\",\"detail\":\"x\","
          "\"dot\":\"orange\",\"kind\":\"deleted\",\"title\":\"y\"}],"
          "\"version\":1}");
  f.close();

  ActivityFeed feed(nullptr, feedPath());
  ASSERT_EQ(feed.count(), 1);
  EXPECT_EQ(feed.get(0)["path"].toString(), "");
}

TEST_F(ActivityFeedTest, GetOutOfRangeIsEmpty) {
  ActivityFeed feed(nullptr, feedPath());
  EXPECT_TRUE(feed.get(0).isEmpty());
  EXPECT_TRUE(feed.get(-1).isEmpty());
}
