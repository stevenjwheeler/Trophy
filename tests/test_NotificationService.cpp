/*
 * Trophy - Unit Tests
 * Duplicate-notification suppression (no D-Bus daemon needed: tests the
 * pure predicate used by NotificationService::showNotification).
 */

#include <QCoreApplication>
#include <QFileInfo>
#include <QSignalSpy>
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

namespace TrophyBackend {
class NotificationServiceTest : public ::testing::Test {
protected:
  // Friendship is not inherited by the TEST_F-generated subclass, so
  // state goes through fixture helpers (which are members of the friend).
  static void setLastNotice(NotificationService &svc, uint id,
                            const QString &title) {
    svc.m_lastNotificationId = id;
    svc.m_lastTitle = title;
  }
};
} // namespace TrophyBackend

TEST_F(NotificationServiceTest, DefaultActionCountsAsAuthorize) {
  // State is set directly: going through showNotification would pop a
  // real notice on the dev desktop (tests run with a live session bus).
  // A body-click reported as the "default" action resolves to authorize;
  // explicit keys pass through untouched.
  NotificationService svc;
  setLastNotice(svc, 42, "New Game: TestGame");
  QSignalSpy spy(&svc, &NotificationService::notificationClicked);
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onActionInvoked", Q_ARG(uint, 42), Q_ARG(QString, QString("default"))));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.last()[0].toString(), QString("authorize"));
  spy.clear();
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onActionInvoked", Q_ARG(uint, 42), Q_ARG(QString, QString("block"))));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.last()[0].toString(), QString("block"));
}

TEST_F(NotificationServiceTest, DefaultActionIgnoredOffPrompt) {
  // The default->authorize mapping only applies to the authorize prompt:
  // a stray default action with any other last title passes through.
  NotificationService svc;
  setLastNotice(svc, 42, "Recording Started");
  QSignalSpy spy(&svc, &NotificationService::notificationClicked);
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onActionInvoked", Q_ARG(uint, 42), Q_ARG(QString, QString("default"))));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.last()[0].toString(), QString("default"));
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

TEST(NotificationIconTest, EmptyAndThemeNameResolveToExtractedFile) {
  // The Notify app_icon must be a real file: a bare "trophy" theme name
  // only resolves when the icon is installed to the system, which an
  // AppImage never does (Plasma then shows a blank-page placeholder).
  const QString extracted = "/cache/trophy/icons/trophy.svg";
  EXPECT_EQ(NotificationService::resolveNotificationIcon(QString(), extracted),
            extracted);
  EXPECT_EQ(
      NotificationService::resolveNotificationIcon(QString("trophy"), extracted),
      extracted);
}

TEST(NotificationIconTest, ExplicitIconsPassThrough) {
  // Caller-supplied icons (e.g. dialog-error) are never rewritten, even
  // when an extracted file is available.
  EXPECT_EQ(NotificationService::resolveNotificationIcon(
                QString("dialog-error"), "/cache/trophy/icons/trophy.svg"),
            "dialog-error");
}

TEST(NotificationIconTest, FallsBackToThemeNameWithoutExtractedFile) {
  // Test binaries don't link the qrc, so extraction is unavailable: the
  // mapping must still yield a usable theme name, never an empty icon.
  EXPECT_EQ(NotificationService::resolveNotificationIcon(QString(), QString()),
            "trophy");
  EXPECT_EQ(
      NotificationService::resolveNotificationIcon(QString("trophy"), QString()),
      "trophy");
}

TEST(NotificationIconTest, IconPathContract) {
  // Either an existing absolute file (app/lib with the qrc linked) or
  // empty (test binaries without resources) — never a dangling name.
  const QString path = NotificationService::notificationIconPath();
  if (!path.isEmpty()) {
    EXPECT_TRUE(QFileInfo(path).isAbsolute());
    EXPECT_TRUE(QFileInfo(path).size() > 0);
  }
}
