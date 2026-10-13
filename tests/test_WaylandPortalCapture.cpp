/*
 * Portal start generation: no compositor/portal needed. Completed D-Bus
 * method calls simulate a late reply from a cancelled start attempt.
 */
#include <QCoreApplication>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusPendingReply>
#include <QProcess>
#include <QElapsedTimer>
#include <QThread>
#include <QSignalSpy>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <mutex>
#include <gtest/gtest.h>

#include "capture/WaylandPortalCapture.h"

// Runs in a child process under dbus-run-session. Tests explicitly send
// each Response, so ordering is controlled rather than based on sleeps.
static int fakeSessionCloseCount = 0;
class FakePortalSession : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Session")
public slots:
  void Close() { ++fakeSessionCloseCount; }
};

class FakeScreenCast : public QObject, protected QDBusContext {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.ScreenCast")
public slots:
  QDBusObjectPath CreateSession(const QVariantMap &) { return request("CreateSession"); }
  QDBusObjectPath SelectSources(const QDBusObjectPath &, const QVariantMap &) {
    return request("SelectSources");
  }
  QDBusObjectPath Start(const QDBusObjectPath &, const QString &, const QVariantMap &) {
    return request("Start");
  }
  void Configure(const QString &mode) { m_mode = mode; }
  void ReleaseHeld() {
    if (m_held.type() == QDBusMessage::MethodCallMessage) {
      connection().send(m_held.createReply(QVariantList{
          QVariant::fromValue(QDBusObjectPath("/org/freedesktop/portal/desktop/request/test/held"))}));
      m_held = QDBusMessage();
    }
  }
  bool HasHeld() { return m_held.type() == QDBusMessage::MethodCallMessage; }
  int SessionCloseCount() { return fakeSessionCloseCount; }
  void Respond(const QString &path, uint response, const QVariantMap &results) {
    auto msg = QDBusMessage::createSignal(path, "org.freedesktop.portal.Request", "Response");
    msg.setArguments({response, results});
    QDBusConnection::sessionBus().send(msg);
  }
private:
  QDBusObjectPath request(const QString &stage) {
    if (m_mode == stage + "Hold") {
      setDelayedReply(true);
      m_held = message();
      return QDBusObjectPath("/");
    }
    if (m_mode == stage + "Error") {
      sendErrorReply(QDBusError::Failed, "injected method failure");
      return QDBusObjectPath("/");
    }
    if (m_mode == stage + "Empty") {
      setDelayedReply(true);
      connection().send(message().createReply());
      return QDBusObjectPath("/");
    }
    return QDBusObjectPath(QString("/org/freedesktop/portal/desktop/request/test/r%1")
                              .arg(++m_counter));
  }
  QString m_mode;
  QDBusMessage m_held;
  int m_counter = 0;
};

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--fake-portal")) {
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1")
      return 2;
    FakeScreenCast portal;
    FakePortalSession session;
    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerService("org.freedesktop.portal.Desktop") ||
        !bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                            QDBusConnection::ExportAllSlots) ||
        !bus.registerObject("/test/session", &session, QDBusConnection::ExportAllSlots))
      return 2;
    return app.exec();
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace TrophyBackend {
class WaylandPortalCaptureTest : public ::testing::Test {
protected:
  QProcess fake;
  bool startFake() {
    // Never replace or talk to a real session portal, even if launched
    // directly rather than through the CTest private-bus launcher.
    if (qgetenv("TROPHY_PRIVATE_PORTAL_TESTS") != "1")
      return false;
    const QString address = QString::fromUtf8(qgetenv("DBUS_SESSION_BUS_ADDRESS"));
    if (address.contains("/run/user/") || address.isEmpty())
      return false;
    if (QDBusConnection::sessionBus().interface()->isServiceRegistered(
            "org.freedesktop.portal.Desktop"))
      return false;
    fake.start(QCoreApplication::applicationFilePath(), {"--fake-portal"});
    if (!fake.waitForStarted(3000))
      return false;
    return waitUntil([] {
      return QDBusConnection::sessionBus().interface()->isServiceRegistered(
          "org.freedesktop.portal.Desktop").value();
    });
  }
  void TearDown() override {
    if (fake.state() != QProcess::NotRunning) {
      fake.terminate();
      if (!fake.waitForFinished(3000)) {
        fake.kill();
        fake.waitForFinished(3000);
      }
    }
  }
  template<class Predicate> static bool waitUntil(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
      QCoreApplication::processEvents();
      if (predicate()) return true;
      QThread::msleep(1);
    }
    return false;
  }
  static QDBusMessage control(const QString &method, const QVariantList &args) {
    auto msg = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop", "org.freedesktop.portal.ScreenCast", method);
    msg.setArguments(args);
    return QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
  }
  static void timeout(WaylandPortalCapture &capture) {
    capture.onStartWatchdogTimeout();
  }
  static bool starting(const WaylandPortalCapture &capture) { return capture.m_starting; }
  static void restore(WaylandPortalCapture &capture) {
    capture.m_restoreToken = "saved-token";
    capture.m_restoreTokenUsed = true;
  }
  static void arm(WaylandPortalCapture &capture, quint64 generation,
                  const QString &requestPath) {
    capture.m_starting = true;
    capture.m_startGeneration = generation;
    capture.m_attemptGeneration = generation;
    capture.m_pendingRequestPath = requestPath;
  }

  static void deliverOldCreateReply(WaylandPortalCapture &capture,
                                    quint64 generation) {
    QDBusMessage message = QDBusMessage::createMethodCall(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast", "CreateSession");
    auto *watcher = new QDBusPendingCallWatcher(
        QDBusPendingCall::fromCompletedCall(
            message.createReply(QVariantList{
                QVariant::fromValue(QDBusObjectPath(
                    "/org/freedesktop/portal/desktop/request/old"))})),
        &capture);
    capture.onCreateSessionCallFinished(watcher, generation);
  }

  static QString pendingPath(const WaylandPortalCapture &capture) {
    return capture.m_pendingRequestPath;
  }

  static void cancel(WaylandPortalCapture &capture) {
    capture.m_startWatchdog.start(5000);
    capture.clearStartGuard();
  }
  static quint64 generation(const WaylandPortalCapture &capture) {
    return capture.m_startGeneration;
  }
  static bool watchdogActive(const WaylandPortalCapture &capture) {
    return capture.m_startWatchdog.isActive();
  }
  static void deliverMetadata(WaylandPortalCapture &capture, quint64 request = 0) {
    capture.applySourceMetadata("Old Game", 1234, true, request);
  }
  static void simulateCapturing(WaylandPortalCapture &capture, quint64 request) {
    capture.m_startGeneration = request;
    capture.m_capturing = true;
  }
  static void nativeStop(WaylandPortalCapture &capture, std::function<void()> hook) {
    capture.m_nativeStopOverride = std::move(hook);
  }
  static bool retiring(const WaylandPortalCapture &capture) { return capture.m_retiring.load(); }
  static void enableNative(WaylandPortalCapture &capture, quint64 generation) {
    capture.m_nativeGeneration.store(generation);
    capture.m_nativeEventsEnabled.store(true);
  }
  static void postNative(WaylandPortalCapture &capture, quint64 generation) {
    capture.postNativeReady(generation);
    capture.postNativeError("old native error", generation);
  }
  static void nativeError(WaylandPortalCapture &capture) {
    WaylandPortalCapture::on_stream_state_changed(&capture,
        PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_ERROR, "retired native error");
  }
};
} // namespace TrophyBackend

class PortalTestEnvironment : public ::testing::Environment {
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};
static const auto *portalTestEnvironment =
    testing::AddGlobalTestEnvironment(new PortalTestEnvironment);

using namespace TrophyBackend;

TEST_F(WaylandPortalCaptureTest, StoppedCaptureRejectsLateMetadata) {
  WaylandPortalCapture capture;
  capture.stopCapture();
  deliverMetadata(capture);
  EXPECT_TRUE(capture.sourceLabel().isEmpty());
  EXPECT_EQ(capture.sourcePid(), 0u);
}

TEST_F(WaylandPortalCaptureTest, PreparedStopDoesNotWaitForNativeTeardownAndBlocksRestart) {
  WaylandPortalCapture capture;
  simulateCapturing(capture, 4);
  enableNative(capture, 4);
  QSignalSpy stopped(&capture, &IVideoCaptureSource::captureStopped);
  QSignalSpy ready(&capture, &IVideoCaptureSource::ready);
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  postNative(capture, 4); // Queued before cancellation, delivered after it.
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, release = false;
  nativeStop(capture, [&] {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
  });
  capture.prepareStopCapture();
  EXPECT_FALSE(capture.isCapturing());
  EXPECT_TRUE(retiring(capture));
  EXPECT_EQ(stopped.count(), 1);
  EXPECT_FALSE(watchdogActive(capture));
  std::jthread worker([&] { capture.finishStopCapture(); });
  EXPECT_TRUE(waitUntil([&] { std::lock_guard lock(mutex); return entered; }));
  int beats = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
  heartbeat.start(1);
  EXPECT_TRUE(waitUntil([&] { return beats >= 20; }));
  EXPECT_TRUE(retiring(capture));
  EXPECT_FALSE(capture.startCapture());
  capture.requestScreenShare();
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(capture.initialize(CaptureConfig{}));
  capture.prepareStopCapture(); // Idempotent; no duplicate logical-stop signal.
  nativeError(capture);
  QCoreApplication::processEvents();
  EXPECT_EQ(stopped.count(), 1);
  EXPECT_EQ(ready.count(), 0);
  EXPECT_EQ(errors.count(), 0);
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  worker.join();
  EXPECT_FALSE(retiring(capture));
  CaptureConfig config;
  config.useDmaBuf = false; // No EGL/GPU access in this test.
  EXPECT_TRUE(capture.initialize(config));
  enableNative(capture, 8);
  postNative(capture, 4); // Old queued native generation cannot reach a reuse.
  QCoreApplication::processEvents();
  EXPECT_EQ(ready.count(), 0);
  EXPECT_EQ(errors.count(), 0);
  nativeStop(capture, {});
}

TEST_F(WaylandPortalCaptureTest, PreparedStopCancelsPortalSubscriptionsBeforeWorkerFinish) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString create = pendingPath(capture);
  control("Respond", {create, 0u, QVariantMap{{"session_handle", "/test/session"}}});
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty() && pendingPath(capture) != create; }));
  const QString select = pendingPath(capture);
  capture.prepareStopCapture();
  EXPECT_TRUE(retiring(capture));
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
  EXPECT_TRUE(pendingPath(capture).isEmpty());
  ASSERT_TRUE(waitUntil([] { return control("SessionCloseCount", {}).arguments().value(0).toInt() == 1; }));
  control("Respond", {select, 0u, QVariantMap{}});
  QCoreApplication::processEvents();
  EXPECT_EQ(errors.count(), 0);
  EXPECT_FALSE(capture.startCapture());
  std::jthread worker([&] { capture.finishStopCapture(); });
  worker.join();
  EXPECT_FALSE(retiring(capture));
}

TEST_F(WaylandPortalCaptureTest, RestartedCaptureRejectsPriorMetadataGeneration) {
  WaylandPortalCapture capture;
  simulateCapturing(capture, 4);
  deliverMetadata(capture, 2);
  EXPECT_TRUE(capture.sourceLabel().isEmpty());
  EXPECT_EQ(capture.sourcePid(), 0u);
  deliverMetadata(capture, 4);
  EXPECT_EQ(capture.sourceLabel(), "Old Game");
  EXPECT_EQ(capture.sourcePid(), 1234u);
  // No native stream was created; teardown takes its null-resource path.
  capture.stopCapture();
}

TEST_F(WaylandPortalCaptureTest, LateCreateReplyCannotReplaceRetriedRequest) {
  WaylandPortalCapture capture;
  arm(capture, 4, "/org/freedesktop/portal/desktop/request/new");
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  deliverOldCreateReply(capture, 2);
  EXPECT_EQ(pendingPath(capture),
            "/org/freedesktop/portal/desktop/request/new");
  EXPECT_EQ(errors.count(), 0);
}

TEST_F(WaylandPortalCaptureTest, CancelStopsWatchdogAndInvalidatesOldReply) {
  WaylandPortalCapture capture;
  arm(capture, 6, "/org/freedesktop/portal/desktop/request/old");
  cancel(capture);
  EXPECT_EQ(generation(capture), 7u);
  EXPECT_FALSE(watchdogActive(capture));
  EXPECT_TRUE(pendingPath(capture).isEmpty());

  arm(capture, 8, "/org/freedesktop/portal/desktop/request/new");
  deliverOldCreateReply(capture, 6);
  EXPECT_EQ(pendingPath(capture),
            "/org/freedesktop/portal/desktop/request/new");
}

TEST_F(WaylandPortalCaptureTest, RealBusResponsesAdvanceCreateSelectAndStart) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString create = pendingPath(capture);
  control("Respond", {create, 0u, QVariantMap{{"session_handle", "/test/session"}}});
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty() && pendingPath(capture) != create; }));
  const QString select = pendingPath(capture);
  control("Respond", {select, 0u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty() && pendingPath(capture) != select; }));
  control("Respond", {pendingPath(capture), 0u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 1; }));
  EXPECT_TRUE(errors.first()[0].toString().contains("No PipeWire streams"));
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
}

TEST_F(WaylandPortalCaptureTest, TimedOutRequestResponseCannotConsumeRetry) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString old = pendingPath(capture);
  timeout(capture);
  ASSERT_EQ(errors.count(), 1);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString current = pendingPath(capture);
  control("Respond", {old, 0u, QVariantMap{{"session_handle", "/test/old"}}});
  QCoreApplication::processEvents();
  EXPECT_EQ(pendingPath(capture), current);
  control("Respond", {current, 1u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 2; }));
  EXPECT_FALSE(starting(capture));
}

TEST_F(WaylandPortalCaptureTest, EmptyMethodReplyClearsGuardAndAllowsRetry) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  control("Configure", {"CreateSessionEmpty"});
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 1; }));
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
  control("Configure", {""});
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
}

TEST_F(WaylandPortalCaptureTest, HeldMethodReplyCannotReplaceNewAttempt) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  control("Configure", {"CreateSessionHold"});
  WaylandPortalCapture capture;
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([] {
    const auto reply = control("HasHeld", {});
    return !reply.arguments().isEmpty() && reply.arguments().first().toBool();
  }));
  timeout(capture);
  control("Configure", {""});
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString current = pendingPath(capture);
  control("ReleaseHeld", {});
  // A round-trip on the same sender plus an event-loop turn delivers the
  // old method reply before checking whether it stole the current path.
  control("HasHeld", {});
  QCoreApplication::processEvents();
  EXPECT_EQ(pendingPath(capture), current);
  control("Respond", {current, 1u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return !starting(capture); }));
}

TEST_F(WaylandPortalCaptureTest, SelectMethodErrorStopsWatchdogAndAllowsRetry) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  control("Configure", {"SelectSourcesError"});
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  control("Respond", {pendingPath(capture), 0u,
                      QVariantMap{{"session_handle", "/test/session"}}});
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 1; }));
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
  control("Configure", {""});
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
}

TEST_F(WaylandPortalCaptureTest, SavedTokenRejectionEmitsStaleInsteadOfPickerRetry) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  restore(capture);
  QSignalSpy stale(&capture, &IVideoCaptureSource::restoreTokenStale);
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString create = pendingPath(capture);
  control("Respond", {create, 0u, QVariantMap{{"session_handle", "/test/session"}}});
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty() && pendingPath(capture) != create; }));
  control("Respond", {pendingPath(capture), 1u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return stale.count() == 1; }));
  EXPECT_EQ(stale.first()[0].toString(), "saved-token");
  EXPECT_EQ(errors.count(), 0);
  EXPECT_FALSE(starting(capture));
  EXPECT_TRUE(pendingPath(capture).isEmpty());
  EXPECT_FALSE(watchdogActive(capture));
}

TEST_F(WaylandPortalCaptureTest, SuccessfulCreateWithoutSessionHandleFailsCleanly) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  control("Respond", {pendingPath(capture), 0u, QVariantMap{}});
  ASSERT_TRUE(waitUntil([&] { return errors.count() == 1; }));
  EXPECT_TRUE(errors.first()[0].toString().contains("no session handle"));
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
  timeout(capture);
  EXPECT_EQ(errors.count(), 1); // No later duplicate terminal error.
}

TEST_F(WaylandPortalCaptureTest, StopDuringSourceSelectionIgnoresOldResponse) {
  if (!startFake()) GTEST_SKIP() << "requires private dbus-run-session bus";
  WaylandPortalCapture capture;
  QSignalSpy errors(&capture, &IVideoCaptureSource::captureError);
  capture.requestScreenShare();
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty(); }));
  const QString create = pendingPath(capture);
  control("Respond", {create, 0u, QVariantMap{{"session_handle", "/test/session"}}});
  ASSERT_TRUE(waitUntil([&] { return !pendingPath(capture).isEmpty() && pendingPath(capture) != create; }));
  const QString select = pendingPath(capture);
  capture.stopCapture();
  ASSERT_TRUE(waitUntil([] {
    const auto reply = control("SessionCloseCount", {});
    return !reply.arguments().isEmpty() && reply.arguments().first().toInt() == 1;
  }));
  control("Respond", {select, 0u, QVariantMap{}});
  control("HasHeld", {});
  QCoreApplication::processEvents();
  EXPECT_EQ(errors.count(), 0);
  EXPECT_FALSE(starting(capture));
  EXPECT_FALSE(watchdogActive(capture));
  EXPECT_TRUE(pendingPath(capture).isEmpty());
}

#include "test_WaylandPortalCapture.moc"
