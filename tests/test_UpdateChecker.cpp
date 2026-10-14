/*
 * Trophy - Unit Tests
 * UpdateChecker: release-tag parsing and version comparison
 * (pure statics; no network involved).
 */

#include <QCoreApplication>
#include <QSignalSpy>
#include <gtest/gtest.h>

#include "core/UpdateChecker.h"

using namespace TrophyBackend;

class UpdateCheckerTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new UpdateCheckerTestEnvironment);

TEST(UpdateCheckerTest, ParsesReleaseTags) {
  int major = 0, minor = 0, patch = 0;
  EXPECT_TRUE(UpdateChecker::parseVersion("v0.9.0", &major, &minor, &patch));
  EXPECT_EQ(major, 0);
  EXPECT_EQ(minor, 9);
  EXPECT_EQ(patch, 0);

  // The "v" prefix is optional.
  EXPECT_TRUE(UpdateChecker::parseVersion("1.2.3", &major, &minor, &patch));
  EXPECT_EQ(major, 1);
  EXPECT_EQ(minor, 2);
  EXPECT_EQ(patch, 3);
}

TEST(UpdateCheckerTest, RejectsNonReleaseShapes) {
  int major = 0, minor = 0, patch = 0;
  // Branch stamps: "<base>-<branch>-<sha>" must never compare
  // against a release tag, or a dev build would read as
  // perpetually behind.
  EXPECT_FALSE(UpdateChecker::parseVersion(
      "0.9.0-dev-abc1234", &major, &minor, &patch));
  EXPECT_FALSE(UpdateChecker::parseVersion(
      "main-abc1234", &major, &minor, &patch));
  EXPECT_FALSE(UpdateChecker::parseVersion("", &major, &minor, &patch));
  EXPECT_FALSE(UpdateChecker::parseVersion("0.9", &major, &minor, &patch));
  EXPECT_FALSE(UpdateChecker::parseVersion("0.9.0.1", &major, &minor, &patch));
  EXPECT_FALSE(UpdateChecker::parseVersion("not-a-version", &major, &minor,
                                            &patch));
  // Prerelease suffixes are not plain release tags.
  EXPECT_FALSE(UpdateChecker::parseVersion("v0.10.0-rc1", &major, &minor,
                                            &patch));
}

TEST(UpdateCheckerTest, CompareVersions) {
  // Newer
  EXPECT_EQ(UpdateChecker::compareVersions(1, 0, 0, 0, 9, 0), 1);
  EXPECT_EQ(UpdateChecker::compareVersions(0, 10, 0, 0, 9, 0), 1);
  EXPECT_EQ(UpdateChecker::compareVersions(0, 9, 1, 0, 9, 0), 1);
  // Older
  EXPECT_EQ(UpdateChecker::compareVersions(0, 8, 0, 0, 9, 0), -1);
  EXPECT_EQ(UpdateChecker::compareVersions(0, 9, 0, 1, 0, 0), -1);
  // Equal
  EXPECT_EQ(UpdateChecker::compareVersions(0, 9, 0, 0, 9, 0), 0);
  // Leading zeros / large components
  EXPECT_EQ(UpdateChecker::compareVersions(1, 0, 0, 0, 9, 0), 1);
  EXPECT_EQ(UpdateChecker::compareVersions(0, 0, 0, 0, 0, 0), 0);
}

TEST(UpdateCheckerTest, EmptyVersionFinishesWithoutNetwork) {
  // No current version (fresh state): checkNow must settle immediately
  // with "nothing newer" and never touch the network — in a sandbox
  // without --share=network the request would only burn the 15s abort
  // timeout. Default-constructed QNAM performs no I/O by itself.
  UpdateChecker checker;
  checker.setCurrentVersion(QString());
  QSignalSpy spy(&checker, &UpdateChecker::checkFinished);
  checker.checkNow();
  ASSERT_EQ(spy.count(), 1);
  EXPECT_FALSE(spy.last()[0].toBool());
  EXPECT_TRUE(spy.last()[1].toString().isEmpty());
  EXPECT_TRUE(spy.last()[2].toString().isEmpty());
}
