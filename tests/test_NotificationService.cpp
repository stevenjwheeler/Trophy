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
  static uint lastId(const NotificationService &svc) {
    return svc.m_lastNotificationId;
  }
  static QString pendingWithdraw(const NotificationService &svc) {
    return svc.m_pendingWithdrawTitle;
  }
  static void recordStackedNotice(NotificationService &svc) {
    svc.noteSent("Ready to clip", "Stacked notice");
  }
  static quint64 beginNotice(NotificationService &svc, const QString &title) {
    return svc.beginReplacingNotification(title);
  }
  static bool adoptReply(NotificationService &svc, quint64 generation, uint id) {
    return svc.adoptNotificationReply(generation, id);
  }
  static bool mayWithdraw(const NotificationService &svc, quint64 generation,
                          uint id) {
    return svc.mayWithdrawNotification(generation, id);
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

TEST_F(NotificationServiceTest, StackedDedupDoesNotChangeAuthorizeOwner) {
  NotificationService svc;
  setLastNotice(svc, 42, "New Game: TestGame");
  recordStackedNotice(svc);
  QSignalSpy spy(&svc, &NotificationService::notificationClicked);
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onActionInvoked", Q_ARG(uint, 42), Q_ARG(QString, QString("default"))));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.last()[0].toString(), QString("authorize"));
  spy.clear();
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onNotificationClosed", Q_ARG(uint, 42), Q_ARG(uint, 2)));
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.last()[0].toString(), QString("dismissed"));
}

TEST_F(NotificationServiceTest, StackedDedupDoesNotLosePendingWithdraw) {
  NotificationService svc;
  setLastNotice(svc, 0, "New Game: TestGame");
  recordStackedNotice(svc);
  svc.closeAuthorizePrompt("TestGame");
  EXPECT_EQ(pendingWithdraw(svc), QString("New Game: TestGame"));
}

TEST_F(NotificationServiceTest, LateReplyCannotStealNewerPrompt) {
  NotificationService svc;
  const auto oldRequest = beginNotice(svc, "New Game: OldGame");
  const auto currentRequest = beginNotice(svc, "New Game: CurrentGame");
  ASSERT_TRUE(adoptReply(svc, currentRequest, 43));
  EXPECT_FALSE(adoptReply(svc, oldRequest, 42));
  EXPECT_EQ(lastId(svc), 43u);
  EXPECT_TRUE(mayWithdraw(svc, oldRequest, 42));
  QSignalSpy spy(&svc, &NotificationService::notificationClicked);
  ASSERT_TRUE(QMetaObject::invokeMethod(
      &svc, "onActionInvoked", Q_ARG(uint, 42), Q_ARG(QString, QString("authorize"))));
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(NotificationServiceTest, OldTimeoutCannotWithdrawReusedReplacementId) {
  NotificationService svc;
  const auto oldRequest = beginNotice(svc, "Recording Saved");
  ASSERT_TRUE(adoptReply(svc, oldRequest, 42));
  const auto currentRequest = beginNotice(svc, "New Game: CurrentGame");
  // Protect the id both before and after the new Notify reply arrives.
  EXPECT_FALSE(mayWithdraw(svc, oldRequest, 42));
  ASSERT_TRUE(adoptReply(svc, currentRequest, 42));
  EXPECT_FALSE(mayWithdraw(svc, oldRequest, 42));
  EXPECT_FALSE(adoptReply(svc, oldRequest, 42));
  EXPECT_EQ(lastId(svc), 42u);
}

TEST_F(NotificationServiceTest, WithdrawOfOldSameTitleDoesNotCancelRetry) {
  NotificationService svc;
  const auto oldRequest = beginNotice(svc, "New Game: TestGame");
  svc.closeAuthorizePrompt("TestGame");
  const auto currentRequest = beginNotice(svc, "New Game: TestGame");
  EXPECT_FALSE(adoptReply(svc, oldRequest, 42));
  EXPECT_TRUE(adoptReply(svc, currentRequest, 43));
  EXPECT_EQ(lastId(svc), 43u);
  EXPECT_TRUE(pendingWithdraw(svc).isEmpty());
}

TEST_F(NotificationServiceTest, CloseLastCancelsPendingReply) {
  NotificationService svc;
  const auto request = beginNotice(svc, "New Game: TestGame");
  svc.closeLastNotification();
  EXPECT_FALSE(adoptReply(svc, request, 42));
  EXPECT_EQ(lastId(svc), 0u);
  EXPECT_TRUE(mayWithdraw(svc, request, 42));
}

TEST_F(NotificationServiceTest, ClosePromptDefersWithdrawWhileReplyInflight) {
  // Async Notify round-trip: the id is still 0 when the game closes.
  // closeAuthorizePrompt must record the title so the reply callback can
  // withdraw the just-published popup instead of letting a dead-name
  // prompt linger (clicks would hit cleared pendings).
  NotificationService svc;
  setLastNotice(svc, 0, "New Game: TestGame");
  svc.closeAuthorizePrompt("TestGame");
  EXPECT_EQ(lastId(svc), 0u);
  EXPECT_EQ(pendingWithdraw(svc), QString("New Game: TestGame"));
}

TEST_F(NotificationServiceTest, ClosePromptIgnoresUnrelatedGame) {
  // Title-matched withdraw must never kill an unrelated notice.
  NotificationService svc;
  setLastNotice(svc, 7, "New Game: GameA");
  svc.closeAuthorizePrompt("GameB");
  EXPECT_EQ(lastId(svc), 7u);
  EXPECT_TRUE(pendingWithdraw(svc).isEmpty());
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
