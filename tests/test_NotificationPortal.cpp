// Private-bus integration only: no live portal, picker or PipeWire daemon.
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>

#include "capture/CaptureFactory.h"
#include "capture/WaylandPortalCapture.h"
#include "core/NotificationService.h"

// Introspect is deliberately never answered. Explicit method dispatch still
// works, making a hidden synchronous introspection a timeout/test failure.
class NoIntrospectionPortal : public QDBusVirtualObject {
public:
  QString introspect(const QString &) const override { return {}; }
  bool handleMessage(const QDBusMessage &msg, const QDBusConnection &bus) override {
    const QString method = msg.member();
    if (msg.interface() == "org.freedesktop.DBus.Introspectable") {
      ++introspectionCount;
      return true;
    }
    if (method == "Counts") {
      bus.send(msg.createReply({introspectionCount, createCount, selectCount,
                                startCount, sessionCloseCount, notifyCount,
                                notificationCloseCount}));
    } else if (method == "LastNotice") {
      bus.send(msg.createReply(lastNotice));
    } else if (method == "ConfigureVersion") {
      versionMode = msg.arguments().value(0).toString();
      bus.send(msg.createReply());
    } else if (method == "Get") {
      if (versionMode == "error") {
        bus.send(msg.createErrorReply("org.freedesktop.DBus.Error.UnknownInterface", "No ScreenCast"));
      } else {
        const QVariant version = versionMode == "malformed" ? QVariant("not-a-version") : QVariant(uint(5));
        const auto reply = msg.createReply(QVariantList{QVariant::fromValue(QDBusVariant(version))});
        QTimer::singleShot(50, this, [bus, reply] { bus.send(reply); });
      }
    } else if (method == "CreateSession" || method == "SelectSources" || method == "Start") {
      if (method == "CreateSession") ++createCount;
      if (method == "SelectSources") ++selectCount;
      if (method == "Start") ++startCount;
      const QString path = QString("/test/request/r%1").arg(++requestCount);
      bus.send(msg.createReply({QVariant::fromValue(QDBusObjectPath(path))}));
      QTimer::singleShot(50, this, [bus, path, method] {
        auto response = QDBusMessage::createSignal(
            path, "org.freedesktop.portal.Request", "Response");
        QVariantMap results;
        if (method == "CreateSession") results["session_handle"] = "/test/session";
        // Empty Start streams intentionally stop before native PipeWire setup.
        response.setArguments({0u, results});
        bus.send(response);
      });
    } else if (method == "Close") {
      ++sessionCloseCount;
      bus.send(msg.createReply());
    } else if (method == "Notify") {
      ++notifyCount;
      lastNotice = msg.arguments();
      const uint replacement = msg.arguments().value(1).toUInt();
      const uint id = replacement ? replacement : ++nextNoticeId;
      // Hold the real method reply while the caller's event loop must tick.
      QTimer::singleShot(150, this, [bus, msg, id] {
        bus.send(msg.createReply(QVariantList{id}));
      });
    } else if (method == "InvokeNoticeAction") {
      auto signal = QDBusMessage::createSignal(
          "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
          "ActionInvoked");
      signal.setArguments(msg.arguments());
      bus.send(signal);
      bus.send(msg.createReply());
    } else if (method == "CloseNotification") {
      ++notificationCloseCount;
      bus.send(msg.createReply());
    } else {
      bus.send(msg.createErrorReply("org.freedesktop.DBus.Error.UnknownMethod", method));
    }
    return true;
  }
private:
  QVariantList lastNotice;
  QString versionMode;
  int introspectionCount = 0, requestCount = 0;
  int createCount = 0, selectCount = 0, startCount = 0;
  int sessionCloseCount = 0, notifyCount = 0, notificationCloseCount = 0;
  uint nextNoticeId = 40;
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--fake-no-introspection")) {
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1") return 2;
    NoIntrospectionPortal portal;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop") ||
        !bus.registerService("org.freedesktop.Notifications") ||
        !bus.registerVirtualObject("/", &portal, QDBusConnection::SubPath)) return 2;
    return app.exec();
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

class NotificationPortalTest : public ::testing::Test {
protected:
  QProcess fake;
  template<class Predicate> static bool waitUntil(Predicate predicate) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 3000) {
      QCoreApplication::processEvents();
      if (predicate()) return true;
      QThread::msleep(1);
    }
    return false;
  }
  bool startFake() {
    const QByteArray address = qgetenv("DBUS_SESSION_BUS_ADDRESS");
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1" ||
        address.isEmpty() || address.contains("/run/user/")) return false;
    auto *bus = QDBusConnection::sessionBus().interface();
    if (bus->isServiceRegistered("org.freedesktop.portal.Desktop").value() ||
        bus->isServiceRegistered("org.freedesktop.Notifications").value()) return false;
    fake.start(QCoreApplication::applicationFilePath(), {"--fake-no-introspection"});
    if (!fake.waitForStarted(3000)) return false;
    return waitUntil([bus] {
      return bus->isServiceRegistered("org.freedesktop.Notifications").value();
    });
  }
  static QVariantList counts() {
    const auto call = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/test/control", "test.Control", "Counts");
    return QDBusConnection::sessionBus().call(call, QDBus::Block, 1000).arguments();
  }
  static void configureVersion(const QString &mode) {
    auto call = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/test/control", "test.Control", "ConfigureVersion");
    call.setArguments({mode});
    QDBusConnection::sessionBus().call(call, QDBus::Block, 1000);
  }
  static QVariantList lastNotice() {
    const auto call = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/test/control", "test.Control", "LastNotice");
    return QDBusConnection::sessionBus().call(call, QDBus::Block, 1000).arguments();
  }
  void TearDown() override {
    if (fake.state() != QProcess::NotRunning) {
      fake.terminate();
      if (!fake.waitForFinished(3000)) { fake.kill(); fake.waitForFinished(3000); }
    }
  }
};

TEST_F(NotificationPortalTest, ScreenCastAndSessionCloseNeverIntrospect) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &TrophyBackend::IVideoCaptureSource::captureError);
  int heartbeats = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
  heartbeat.start(1);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 1; }));
  EXPECT_TRUE(errors.first()[0].toString().contains("No PipeWire streams"));
  capture.stopCapture();
  ASSERT_TRUE(waitUntil([] { return counts().value(4).toInt() == 1; }));
  const auto observed = counts();
  ASSERT_EQ(observed.size(), 7);
  EXPECT_EQ(observed[0].toInt(), 0);
  EXPECT_EQ(observed[1].toInt(), 1);
  EXPECT_EQ(observed[2].toInt(), 1);
  EXPECT_EQ(observed[3].toInt(), 1);
  EXPECT_GT(heartbeats, 10);
}

TEST_F(NotificationPortalTest, DelayedNotifyAndCloseNeverIntrospectOrBlockHeartbeat) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::NotificationService notifications;
  int heartbeats = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
  heartbeat.start(1);
  notifications.showNotification("New Game: Test", "Authorize", "", 0, 2, true);
  // Withdraw during Notify's reply round-trip exercises actual ownership logic.
  notifications.closeAuthorizePrompt("Test");
  ASSERT_TRUE(waitUntil([] { return counts().value(6).toInt() == 1; }));
  const auto observed = counts();
  ASSERT_EQ(observed.size(), 7);
  EXPECT_EQ(observed[0].toInt(), 0);
  EXPECT_EQ(observed[5].toInt(), 1);
  EXPECT_GT(heartbeats, 10);
}

TEST_F(NotificationPortalTest, CaptureHintReplacesPromptAndWithdrawsOnCompletion) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::NotificationService notifications;
  notifications.showNotification("New Game: Game <One>", "Authorize", "", 0, 2, true);
  // Wait for the delayed prompt id to be adopted before replacing it.
  QElapsedTimer elapsed;
  elapsed.start();
  ASSERT_TRUE(waitUntil([&] { return elapsed.elapsed() >= 250; }));
  notifications.showCaptureSelectionHint("Game <One>");
  ASSERT_TRUE(waitUntil([] { return counts().value(5).toInt() == 2; }));
  const auto notice = lastNotice();
  ASSERT_EQ(notice.size(), 8);
  EXPECT_EQ(notice[1].toUInt(), 41u);
  EXPECT_EQ(notice[3].toString(), "Select capture: Game <One>");
  EXPECT_TRUE(notice[4].toString().contains("Game &lt;One&gt;"));
  EXPECT_TRUE(qdbus_cast<QStringList>(notice[5]).isEmpty());
  const auto hints = qdbus_cast<QVariantMap>(notice[6]);
  EXPECT_TRUE(hints.value("resident").toBool());
  EXPECT_EQ(hints.value("urgency").value<uchar>(), 2);
  EXPECT_EQ(notice[7].toInt(), 0);
  // Finish while the hint's Notify reply is still held: it must not linger.
  notifications.closeCaptureSelectionHint();
  ASSERT_TRUE(waitUntil([] { return counts().value(6).toInt() == 1; }));
  EXPECT_EQ(counts().value(0).toInt(), 0);
}

TEST_F(NotificationPortalTest, AttentionIsIndependentAndOnlyExplicitClickRequestsWindow) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::NotificationService notifications;
  QSignalSpy attention(&notifications, &TrophyBackend::NotificationService::attentionRequested);
  QSignalSpy regular(&notifications, &TrophyBackend::NotificationService::notificationClicked);
  notifications.showNotification("New Game: Test", "Authorize", "trophy", 0, 2, true);
  notifications.showAttention("Helper <repair>");
  ASSERT_TRUE(waitUntil([] { return counts().value(5).toInt() == 2; }));
  const auto notice = lastNotice();
  ASSERT_EQ(notice.size(), 8);
  EXPECT_EQ(notice[1].toUInt(), 0u); // never replaces authorize
  EXPECT_EQ(notice[3].toString(), "Trophy needs your attention");
  EXPECT_EQ(notice[4].toString(), "Helper &lt;repair&gt;");
  EXPECT_EQ(qdbus_cast<QStringList>(notice[5]),
            (QStringList{"show-attention", "Open Trophy", "default", ""}));
  const auto hints = qdbus_cast<QVariantMap>(notice[6]);
  EXPECT_TRUE(hints.value("resident").toBool());
  EXPECT_EQ(hints.value("urgency").value<uchar>(), 1);
  EXPECT_EQ(notice[7].toInt(), 0);
  EXPECT_EQ(attention.count(), 0);
  // A later completion notice cannot take ownership of the attention action.
  QElapsedTimer elapsed;
  elapsed.start();
  ASSERT_TRUE(waitUntil([&] { return elapsed.elapsed() >= 250; }));
  notifications.showNotification("Recording Saved", "Done", "trophy", 0);
  auto action = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/test/control", "test.Control", "InvokeNoticeAction");
  action.setArguments({42u, QStringLiteral("default")});
  QDBusConnection::sessionBus().call(action, QDBus::Block, 1000);
  ASSERT_TRUE(waitUntil([&] { return attention.count() == 1; }));
  EXPECT_EQ(regular.count(), 0);
  ASSERT_TRUE(waitUntil([] { return counts().value(6).toInt() == 1; }));
  // The now-withdrawn notice cannot reopen Trophy a second time.
  QDBusConnection::sessionBus().call(action, QDBus::Block, 1000);
  QCoreApplication::processEvents();
  EXPECT_EQ(attention.count(), 1);
}

TEST_F(NotificationPortalTest, ResolvedAttentionWithdrawsHeldReplyAndAllowsSameFailureAgain) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::NotificationService notifications;
  QSignalSpy attention(&notifications, &TrophyBackend::NotificationService::attentionRequested);
  notifications.showAttention("Helper needs repair");
  notifications.clearAttention();
  ASSERT_TRUE(waitUntil([] { return counts().value(6).toInt() == 1; }));
  EXPECT_EQ(attention.count(), 0);
  notifications.showAttention("Helper needs repair");
  ASSERT_TRUE(waitUntil([] { return counts().value(5).toInt() == 2; }));
  notifications.clearAttention();
  ASSERT_TRUE(waitUntil([] { return counts().value(6).toInt() == 2; }));
}

TEST_F(NotificationPortalTest, AttentionButtonRequestsWindowAndDisabledServiceSendsNothing) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  TrophyBackend::NotificationService notifications;
  QSignalSpy attention(&notifications, &TrophyBackend::NotificationService::attentionRequested);
  notifications.setEnabled(false);
  notifications.showAttention("Recording error");
  EXPECT_EQ(counts().value(5).toInt(), 0);
  notifications.setEnabled(true);
  notifications.showAttention("Recording error");
  ASSERT_TRUE(waitUntil([] { return counts().value(5).toInt() == 1; }));
  QElapsedTimer elapsed;
  elapsed.start();
  ASSERT_TRUE(waitUntil([&] { return elapsed.elapsed() >= 250; }));
  auto action = QDBusMessage::createMethodCall(
      "org.freedesktop.Notifications", "/test/control", "test.Control", "InvokeNoticeAction");
  action.setArguments({41u, QStringLiteral("show-attention")});
  QDBusConnection::sessionBus().call(action, QDBus::Block, 1000);
  ASSERT_TRUE(waitUntil([&] { return attention.count() == 1; }));
}

TEST_F(NotificationPortalTest, AvailabilityUsesAsyncVersionPropertyWithoutIntrospection) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  QObject context;
  bool completed = false;
  int heartbeats = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
  heartbeat.start(1);
  TrophyBackend::CaptureFactory::requestPipeWireAvailability(&context, [&](bool available) {
    EXPECT_TRUE(available);
    completed = true;
  });
  ASSERT_TRUE(waitUntil([&] { return completed; }));
  EXPECT_TRUE(TrophyBackend::CaptureFactory::isPipeWireAvailable());
  EXPECT_EQ(counts().value(0).toInt(), 0);
  EXPECT_GT(heartbeats, 10);
}

TEST_F(NotificationPortalTest, UnavailableOrMalformedVersionClearsConfirmedAvailability) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  QObject context;
  for (const QString &mode : {QString(), QString("error"), QString("malformed")}) {
    configureVersion(mode);
    bool completed = false;
    TrophyBackend::CaptureFactory::requestPipeWireAvailability(&context, [&](bool available) {
      EXPECT_EQ(available, mode.isEmpty());
      completed = true;
    });
    ASSERT_TRUE(waitUntil([&] { return completed; }));
    EXPECT_EQ(TrophyBackend::CaptureFactory::isPipeWireAvailable(), mode.isEmpty());
  }
  EXPECT_EQ(counts().value(0).toInt(), 0);
}

TEST_F(NotificationPortalTest, DestroyedAvailabilityContextCancelsDelivery) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  bool delivered = false;
  auto *context = new QObject;
  TrophyBackend::CaptureFactory::requestPipeWireAvailability(context, [&](bool) {
    delivered = true;
  });
  delete context;
  // A surviving later request establishes that delayed replies were processed.
  QObject survivor;
  bool completed = false;
  TrophyBackend::CaptureFactory::requestPipeWireAvailability(&survivor, [&](bool) {
    completed = true;
  });
  ASSERT_TRUE(waitUntil([&] { return completed; }));
  EXPECT_FALSE(delivered);
}
