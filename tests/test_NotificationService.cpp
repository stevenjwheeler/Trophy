/*
 * Trophy - Unit Tests
 * Duplicate-notification suppression (no D-Bus daemon needed: tests the
 * pure predicate used by NotificationService::showNotification).
 */

#include <QCoreApplication>
#include <QVariantMap>
#include <gtest/gtest.h>

#include "core/NotificationService.h"

using namespace TrophyBackend;

class NotificationServiceTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new NotificationServiceTestEnvironment);

TEST(NotificationDedupTest, SuppressesIdenticalWithinCooldown) {
  EXPECT_TRUE(NotificationService::isRepeatSuppressed(
      "Storage limit exceeded", "body", "Storage limit exceeded", "body",
      /*elapsedMs=*/1000));
  EXPECT_TRUE(NotificationService::isRepeatSuppressed(
      "Storage limit exceeded", "body", "Storage limit exceeded", "body",
      /*elapsedMs=*/0));
}

TEST(NotificationDedupTest, AllowsAfterCooldown) {
  EXPECT_FALSE(NotificationService::isRepeatSuppressed(
      "Storage limit exceeded", "body", "Storage limit exceeded", "body",
      /*elapsedMs=*/6000));
  EXPECT_FALSE(NotificationService::isRepeatSuppressed(
      "Storage limit exceeded", "body", "Storage limit exceeded", "body",
      /*elapsedMs=*/-1));
}

TEST(NotificationDedupTest, AllowsDifferentContent) {
  EXPECT_FALSE(NotificationService::isRepeatSuppressed(
      "Storage limit exceeded", "body A", "Storage limit exceeded", "body B",
      /*elapsedMs=*/100));
  EXPECT_FALSE(NotificationService::isRepeatSuppressed(
      "Title A", "body", "Title B", "body", /*elapsedMs=*/100));
}

TEST(NotificationHintsTest, CriticalNoticeIsNotResidentWithoutAuthorize) {
  // Transient notices (clip saved, recording started/stopped) must expire
  // fully: with the resident hint set, Plasma keeps every one until
  // manually dismissed. Only the authorize prompt stays resident.
  const QVariantMap transient =
      NotificationService::notificationHints(2, false);
  EXPECT_EQ(transient.value("urgency").value<uchar>(), 2);
  EXPECT_FALSE(transient.contains("resident"));

  const QVariantMap authorize =
      NotificationService::notificationHints(2, true);
  EXPECT_EQ(authorize.value("urgency").value<uchar>(), 2);
  EXPECT_TRUE(authorize.contains("resident"));
}

TEST(NotificationFormatTest, FormatsDurations) {
  EXPECT_EQ(NotificationService::formatDuration(0), "0:00");
  EXPECT_EQ(NotificationService::formatDuration(-5), "0:00");
  EXPECT_EQ(NotificationService::formatDuration(999), "0:00");
  EXPECT_EQ(NotificationService::formatDuration(5000), "0:05");
  EXPECT_EQ(NotificationService::formatDuration(154000), "2:34");
  EXPECT_EQ(NotificationService::formatDuration(3599000), "59:59");
  EXPECT_EQ(NotificationService::formatDuration(3723000), "1:02:03");
}
