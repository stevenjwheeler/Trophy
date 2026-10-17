/*
 * Trophy - Unit Tests
 * KWin snapshot parsing, WaylandWindowMonitor focus resolution, and
 * WindowService delegation. No compositor or D-Bus session needed:
 * snapshots are fed straight into ingestSnapshot().
 */

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDir>
#include <QJsonDocument>
#include <QSet>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <future>
#include <atomic>

#include "core/WindowService.h"
#include "detection/GnomeBackend.h"
#include "detection/HyprlandBackend.h"
#include "detection/KWinBackend.h"
#include "detection/SwayBackend.h"
#include "detection/WindowInfo.h"
#include "detection/WaylandWindowMonitor.h"
#include "detection/WaylandWindowMonitor.h"
#include "detection/WindowInfo.h"

using namespace TrophyBackend;

class WindowMonitorTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new WindowMonitorTestEnvironment);

namespace TrophyBackend {
class WindowMonitorWorkerTest : public ::testing::Test {
protected:
  static void arm(WaylandWindowMonitor &monitor, std::function<void()> worker) {
    monitor.m_monitoring = true;
    ++monitor.m_generation;
    monitor.m_pollOverride = std::move(worker);
    monitor.checkFocusedApp();
  }
  static void collect(WaylandWindowMonitor &monitor) { monitor.collectRetired(); }
  static bool retired(WaylandWindowMonitor &monitor) { return !monitor.m_retired.empty(); }
  static std::shared_ptr<std::atomic_bool> attachPendingGnomeSetup(WaylandWindowMonitor &monitor) {
    monitor.m_monitoring = true;
    monitor.m_gnome = std::make_shared<GnomeBackend>();
    monitor.m_gnome->m_setupPending->store(true);
    return monitor.m_gnome->m_setupPending;
  }
  static void beginHealthCheck(WaylandWindowMonitor &monitor) {
    monitor.m_healthTimer.start(0);
  }
  static std::shared_ptr<HyprlandBackend> attach(WaylandWindowMonitor &monitor) {
    monitor.m_hyprland = std::make_shared<HyprlandBackend>();
    const auto generation = monitor.m_generation;
    QObject::connect(monitor.m_hyprland.get(), &HyprlandBackend::windowsUpdated,
        &monitor, [&monitor, generation] {
          if (monitor.m_generation == generation) monitor.onBackendWindowsUpdated();
        }, Qt::QueuedConnection);
    return monitor.m_hyprland;
  }
};

TEST_F(WindowMonitorWorkerTest, HealthCheckSettlesWithoutCallingTrackingWorking) {
  WaylandWindowMonitor monitor;
  EXPECT_FALSE(monitor.trackingReady());
  EXPECT_FALSE(monitor.trackingChecking());
  QSignalSpy changed(&monitor, &WaylandWindowMonitor::windowsChanged);
  beginHealthCheck(monitor);
  EXPECT_TRUE(monitor.trackingChecking());
  QCoreApplication::processEvents();
  EXPECT_FALSE(monitor.trackingChecking());
  EXPECT_FALSE(monitor.trackingReady());
  EXPECT_GT(changed.count(), 0);
}

TEST_F(WindowMonitorWorkerTest, PendingGnomeSetupIsRetainedUntilSettled) {
  WaylandWindowMonitor monitor;
  const auto pending = attachPendingGnomeSetup(monitor);
  monitor.stopMonitoring();
  EXPECT_TRUE(retired(monitor));
  collect(monitor);
  EXPECT_TRUE(retired(monitor));
  pending->store(false);
  collect(monitor);
  EXPECT_FALSE(retired(monitor));
}

TEST_F(WindowMonitorWorkerTest, StopClearsHealthCheckAndReadiness) {
  WaylandWindowMonitor monitor;
  arm(monitor, [] {});
  // A running monitoring loop alone is not evidence of a working backend.
  EXPECT_TRUE(monitor.isMonitoring());
  EXPECT_FALSE(monitor.trackingReady());
  beginHealthCheck(monitor);
  monitor.stopMonitoring();
  EXPECT_FALSE(monitor.trackingChecking());
  EXPECT_FALSE(monitor.trackingReady());
}

TEST(KWinBackendHealthTest, ValidEmptySnapshotProvesReportingButMalformedDoesNot) {
  KWinBackend backend;
  EXPECT_FALSE(backend.hasSnapshot());
  backend.ReportWindows("not json");
  EXPECT_FALSE(backend.hasSnapshot());
  backend.ReportWindows("{}");
  EXPECT_FALSE(backend.hasSnapshot());
  backend.ReportWindows("[]");
  EXPECT_TRUE(backend.hasSnapshot());
  EXPECT_TRUE(backend.windows().isEmpty());
  backend.stop();
  EXPECT_FALSE(backend.hasSnapshot());
}

TEST_F(WindowMonitorWorkerTest, GatedStopRestartKeepsGuiResponsiveAndRejectsOldDelivery) {
  WaylandWindowMonitor monitor;
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic_bool finished{false};
  arm(monitor, [&] { entered.set_value(); gate.wait(); finished = true; });
  ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  auto backend = attach(monitor);
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(
      R"([{"id":6,"appId":"old","focused":true}])"));
  ASSERT_EQ(monitor.focusedAppId(), "old");
  std::weak_ptr<HyprlandBackend> lifetime = backend;
  emit backend->windowsUpdated(); // queued against the OLD generation
  monitor.stopMonitoring();
  backend.reset();
  EXPECT_TRUE(monitor.windows().isEmpty());
  EXPECT_TRUE(monitor.focusedAppId().isEmpty());
  EXPECT_FALSE(finished.load());
  EXPECT_FALSE(lifetime.expired());
  EXPECT_TRUE(retired(monitor));
  // Fresh generation can progress even though the retired poll is gated.
  arm(monitor, [] {});
  auto wins = KWinBackend::parseSnapshot(R"([{"id":7,"appId":"fresh","focused":true}])");
  monitor.ingestSnapshot(wins);
  int heartbeat = 0;
  QTimer::singleShot(0, &monitor, [&] { ++heartbeat; });
  QCoreApplication::processEvents();
  EXPECT_EQ(heartbeat, 1);
  EXPECT_EQ(monitor.windows().size(), wins.size());
  release.set_value();
  monitor.stopMonitoring();
  QElapsedTimer timeout;
  timeout.start();
  while (retired(monitor) && timeout.elapsed() < 2000) {
    collect(monitor);
    QThread::yieldCurrentThread();
  }
  EXPECT_FALSE(retired(monitor));
  EXPECT_TRUE(lifetime.expired());
}

class KWinWorkerTest : public ::testing::Test {
protected:
  static void setup(KWinBackend &backend, bool allowRepair = true) {
    backend.m_active = true;
    backend.queueSetup(allowRepair);
  }
  static void mock(std::function<bool(const QString &, const QStringList &, QString *)> tool) {
    KWinBackend::s_toolOverride = std::move(tool);
    KWinBackend::s_restartOverride = [] {};
  }
  static void reset() { KWinBackend::s_toolOverride = {}; KWinBackend::s_restartOverride = {}; }
  static std::shared_future<void> fence() { return KWinBackend::enqueueOperation([] {}); }
  static void onRestart(std::function<void()> callback) { KWinBackend::s_restartOverride = std::move(callback); }
  static void armStatus(KWinBackend &backend) {
    backend.m_active = true;
    ++backend.m_setupGeneration;
    KWinBackend::s_enabledCache.store(1);
  }
  static bool statusSettled(KWinBackend &backend) {
    return backend.m_statusFuture.valid() &&
           backend.m_statusFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  }
};

TEST_F(KWinWorkerTest, StartupDoesNotRepairMissingOrDisabledScriptWithoutConsent) {
  const auto oldData = qgetenv("XDG_DATA_HOME");
  const auto restore = qScopeGuard([&] { qputenv("XDG_DATA_HOME", oldData); reset(); });
  for (const bool installed : {false, true}) {
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    qputenv("XDG_DATA_HOME", temp.path().toUtf8());
    if (installed) {
      QDir().mkpath(KWinBackend::installedScriptDir());
      QFile metadata(KWinBackend::installedScriptDir() + "/metadata.json");
      ASSERT_TRUE(metadata.open(QIODevice::WriteOnly));
      metadata.write("existing");
    }
    std::atomic<int> writes{0}, restarts{0};
    mock([&](const QString &tool, const QStringList &, QString *output) {
      if (tool.startsWith("kreadconfig")) {
        if (output) *output = installed ? "false" : "true";
        return true;
      }
      ++writes;
      return false;
    });
    onRestart([&] { ++restarts; });
    {
      KWinBackend backend;
      setup(backend, false);
      EXPECT_EQ(fence().wait_for(std::chrono::seconds(2)), std::future_status::ready);
      EXPECT_EQ(KWinBackend::isScriptInstalled(), installed);
      EXPECT_EQ(KWinBackend::isScriptEnabled(), !installed);
      EXPECT_FALSE(backend.hasSnapshot());
      EXPECT_EQ(writes.load(), 0);
      EXPECT_EQ(restarts.load(), 0);
    }
  }
}

TEST_F(KWinWorkerTest, ExplicitRepairStillInstallsAndEnablesAfterStartupRefusal) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto oldData = qgetenv("XDG_DATA_HOME");
  const auto oldSource = qgetenv("TROPHY_KWIN_SCRIPT_DIR");
  const auto oldPath = qgetenv("PATH");
  const auto restore = qScopeGuard([&] {
    qputenv("XDG_DATA_HOME", oldData);
    qputenv("TROPHY_KWIN_SCRIPT_DIR", oldSource);
    qputenv("PATH", oldPath);
    reset();
  });
  qputenv("XDG_DATA_HOME", (temp.path() + "/data").toUtf8());
  qputenv("TROPHY_KWIN_SCRIPT_DIR", (temp.path() + "/source").toUtf8());
  qputenv("PATH", "");
  QDir().mkpath(temp.path() + "/source/contents/code");
  for (const auto &name : {QString("metadata.json"), QString("contents/code/main.js")}) {
    QFile file(temp.path() + "/source/" + name);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("test helper");
  }
  std::atomic<int> writes{0}, restarts{0};
  mock([&](const QString &tool, const QStringList &, QString *output) {
    if (tool.startsWith("kreadconfig")) {
      if (output) *output = "false";
      return true;
    }
    if (tool.startsWith("kwriteconfig")) { ++writes; return true; }
    return false;
  });
  onRestart([&] { ++restarts; });
  KWinBackend backend;
  setup(backend, false);
  EXPECT_EQ(fence().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_FALSE(KWinBackend::isScriptInstalled());
  EXPECT_EQ(writes.load(), 0);
  backend.stop();
  setup(backend, true);
  EXPECT_EQ(fence().wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(KWinBackend::isScriptInstalled());
  EXPECT_TRUE(KWinBackend::isScriptEnabled());
  EXPECT_EQ(writes.load(), 1);
  EXPECT_EQ(restarts.load(), 1);
}

TEST_F(KWinWorkerTest, ExternalDisableClearsCachedHealthAndWindowsWithoutGuiWait) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<int> calls{0};
  std::atomic<bool> enabled{false};
  mock([&](const QString &, const QStringList &, QString *output) {
    if (++calls == 1) entered.set_value();
    gate.wait();
    if (output) *output = enabled.load() ? "true" : "false";
    return true;
  });
  {
    KWinBackend backend;
    armStatus(backend);
    backend.ReportWindows(R"([{"id":7,"appId":"game","focused":true}])");
    QSignalSpy changed(&backend, &KWinBackend::windowsUpdated);
    backend.refreshStatus();
    EXPECT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    for (int i = 0; i < 20; ++i) backend.refreshStatus();
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(backend.hasSnapshot());
    int heartbeat = 0;
    QTimer::singleShot(0, &backend, [&] { ++heartbeat; });
    QCoreApplication::processEvents();
    EXPECT_EQ(heartbeat, 1);
    release.set_value();
    QElapsedTimer timeout;
    timeout.start();
    while (!statusSettled(backend) && timeout.elapsed() < 2000) QThread::yieldCurrentThread();
    EXPECT_TRUE(statusSettled(backend));
    backend.refreshStatus();
    EXPECT_FALSE(KWinBackend::isScriptEnabled());
    EXPECT_FALSE(backend.hasSnapshot());
    EXPECT_TRUE(backend.windows().isEmpty());
    EXPECT_EQ(changed.count(), 1);
    enabled = true;
    backend.refreshStatus();
    timeout.restart();
    while (!statusSettled(backend) && timeout.elapsed() < 2000) QThread::yieldCurrentThread();
    EXPECT_TRUE(statusSettled(backend));
    backend.refreshStatus();
    EXPECT_TRUE(KWinBackend::isScriptEnabled());
    EXPECT_FALSE(backend.hasSnapshot()); // needs a NEW report, not the old cache
    EXPECT_EQ(changed.count(), 2);
    backend.ReportWindows("[]");
    EXPECT_TRUE(backend.hasSnapshot());
  }
  reset();
}

TEST_F(KWinWorkerTest, StatusReplyFromBeforeRestartCannotDisableReplacement) {
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic<bool> first{true};
  mock([&](const QString &, const QStringList &, QString *output) {
    if (first.exchange(false)) entered.set_value();
    gate.wait();
    if (output) *output = "false";
    return true;
  });
  {
    KWinBackend backend;
    armStatus(backend);
    backend.refreshStatus();
    EXPECT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    backend.stop();
    armStatus(backend);
    backend.ReportWindows("[]");
    release.set_value();
    QElapsedTimer timeout;
    timeout.start();
    while (!statusSettled(backend) && timeout.elapsed() < 2000) QThread::yieldCurrentThread();
    EXPECT_TRUE(statusSettled(backend));
    backend.refreshStatus();
    EXPECT_TRUE(KWinBackend::isScriptEnabled());
    EXPECT_TRUE(backend.hasSnapshot());
  }
  reset();
}

TEST_F(KWinWorkerTest, SetupThenOffSettlesInOrderWithoutGuiWait) {
  QTemporaryDir temp;
  ASSERT_TRUE(temp.isValid());
  const auto oldData = qgetenv("XDG_DATA_HOME");
  const auto oldSource = qgetenv("TROPHY_KWIN_SCRIPT_DIR");
  const auto oldPath = qgetenv("PATH");
  qputenv("XDG_DATA_HOME", (temp.path() + "/data").toUtf8());
  qputenv("TROPHY_KWIN_SCRIPT_DIR", (temp.path() + "/source").toUtf8());
  qputenv("PATH", "");
  QDir().mkpath(temp.path() + "/source/contents/code");
  for (const auto &relative : {QString("metadata.json"), QString("contents/code/main.js")}) {
    QFile file(temp.path() + "/source/" + relative);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("mock");
  }
  std::promise<void> entered, release;
  auto gate = release.get_future().share();
  std::atomic_bool gated{false};
  QStringList writes;
  mock([&](const QString &tool, const QStringList &args, QString *) {
    if (tool == "kreadconfig6" && !gated.exchange(true)) {
      entered.set_value(); gate.wait();
    }
    if (tool.startsWith("kwriteconfig")) { writes.append(args.last()); return true; }
    return false;
  });
  {
    KWinBackend backend;
    setup(backend);
    EXPECT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    backend.stop();
    WaylandWindowMonitor monitor;
    monitor.uninstallBackend();
    auto removal = fence();
    EXPECT_TRUE(backend.hasPendingWork());
    EXPECT_NE(removal.wait_for(std::chrono::seconds(0)), std::future_status::ready);
    int heartbeat = 0;
    QTimer::singleShot(0, &backend, [&] { ++heartbeat; });
    QCoreApplication::processEvents();
    EXPECT_EQ(heartbeat, 1);
    release.set_value();
    removal.wait();
    EXPECT_EQ(writes, (QStringList{"true", "false"}));
    EXPECT_FALSE(KWinBackend::isScriptInstalled());
    EXPECT_FALSE(KWinBackend::isScriptEnabled());
  }
  reset();
  qputenv("XDG_DATA_HOME", oldData);
  qputenv("TROPHY_KWIN_SCRIPT_DIR", oldSource);
  qputenv("PATH", oldPath);
}
} // namespace TrophyBackend

static const char *kSnapshot = R"([
  {"id": 101, "appId": "steam_app_730", "title": "Counter-Strike 2",
   "pid": 4242, "fullscreen": true, "focused": true},
  {"id": 102, "appId": "firefox", "title": "Mozilla Firefox",
   "pid": 1234, "fullscreen": false, "focused": false}
])";

// --- KWinBackend::parseSnapshot ---

TEST(KWinBackendParseTest, ValidSnapshotParsesAllFields) {
  const QList<WindowInfo> wins = KWinBackend::parseSnapshot(kSnapshot);
  ASSERT_EQ(wins.size(), 2);
  EXPECT_EQ(wins[0].id, 101u);
  EXPECT_EQ(wins[0].appId, "steam_app_730");
  EXPECT_EQ(wins[0].title, "Counter-Strike 2");
  EXPECT_EQ(wins[0].pid, 4242);
  EXPECT_TRUE(wins[0].fullscreen);
  EXPECT_TRUE(wins[0].focused);
  EXPECT_FALSE(wins[1].fullscreen);
  EXPECT_FALSE(wins[1].focused);
}

TEST(KWinBackendParseTest, GarbageYieldsEmptyList) {
  EXPECT_TRUE(KWinBackend::parseSnapshot("not json").isEmpty());
  EXPECT_TRUE(KWinBackend::parseSnapshot("{\"id\": 1}").isEmpty());
  EXPECT_TRUE(KWinBackend::parseSnapshot("").isEmpty());
}

TEST(KWinBackendParseTest, EmptyArrayYieldsEmptyList) {
  EXPECT_TRUE(KWinBackend::parseSnapshot("[]").isEmpty());
}

TEST(KWinBackendParseTest, MissingFieldsDefaultSafely) {
  const QList<WindowInfo> wins =
      KWinBackend::parseSnapshot(R"([{"id": 7, "title": "Bare"}])");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].id, 7u);
  EXPECT_EQ(wins[0].title, "Bare");
  EXPECT_TRUE(wins[0].appId.isEmpty());
  EXPECT_EQ(wins[0].pid, 0);
  EXPECT_FALSE(wins[0].fullscreen);
  EXPECT_FALSE(wins[0].focused);
}

TEST(KWinBackendParseTest, FullyEmptyEntriesSkipped) {
  const QList<WindowInfo> wins =
      KWinBackend::parseSnapshot(R"([{"id": 0}, {"id": 9, "title": "Kept"}])");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].id, 9u);
}

TEST(KWinBackendParseTest, NewScriptFieldsAreAdditive) {
  // The rewritten trophy-monitor script emits extra keys (stable string
  // key, resourceName, desktopFileName, minimized). Older parsing must
  // keep reading the known fields and ignore the rest.
  const QList<WindowInfo> wins = KWinBackend::parseSnapshot(
      R"([{"id": 0, "key": "window/abc-123", "appId": "deadlock",
           "resourceName": "deadlock", "desktopFileName": "steam_app_1422450",
           "title": "Deadlock", "pid": 4242, "fullscreen": true,
           "focused": true, "minimized": false,
           "x": 0, "y": 0, "width": 1920, "height": 1080,
           "outputWidth": 1920, "outputHeight": 1080}])");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].appId, "deadlock");
  EXPECT_EQ(wins[0].title, "Deadlock");
  EXPECT_EQ(wins[0].pid, 4242);
  EXPECT_TRUE(wins[0].fullscreen);
  EXPECT_TRUE(wins[0].focused);
  EXPECT_EQ(wins[0].width, 1920);
  EXPECT_EQ(wins[0].outputWidth, 1920);
}

TEST(KWinBackendParseTest, OversizedSnapshotDroppedWithoutParse) {
  // ReportWindows bounds what is parsed so a runaway snapshot cannot
  // balloon the GUI thread (~1 MiB cap). Oversized input leaves the
  // previous snapshot untouched and emits nothing.
  KWinBackend backend;
  QSignalSpy spy(&backend, &KWinBackend::windowsUpdated);
  const QString huge(1024 * 1024 + 64, QChar('x'));
  backend.ReportWindows(huge);
  EXPECT_TRUE(backend.windows().isEmpty());
  EXPECT_EQ(spy.count(), 0);
}

TEST(KWinBackendParseTest, StringKeyParsedForIdentityChain) {
  // The script's stable key must survive parsing so the monitor can
  // derive deterministic ids from it (see WindowMonitorTest below).
  const QList<WindowInfo> wins = KWinBackend::parseSnapshot(
      R"([{"id": 0, "key": "window/abc-123", "title": "Game"}])");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].key, "window/abc-123");
}

// --- WaylandWindowMonitor ---

class WindowMonitorTest : public ::testing::Test {
protected:
  WaylandWindowMonitor monitor;
};

TEST_F(WindowMonitorTest, IngestResolvesFocusedApp) {
  QSignalSpy focusSpy(&monitor, &WaylandWindowMonitor::focusedAppChanged);
  QSignalSpy windowsSpy(&monitor, &WaylandWindowMonitor::windowsChanged);

  monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));

  EXPECT_EQ(monitor.focusedAppId(), "steam_app_730");
  EXPECT_EQ(monitor.windows().size(), 2);
  EXPECT_EQ(focusSpy.count(), 1);
  EXPECT_EQ(windowsSpy.count(), 1);
}

TEST_F(WindowMonitorTest, KeyedZeroIdsAreStableAcrossReorder) {
  // Regression for id-0 matching the first row: zero-id rows WITH a
  // stable key get deterministic ids that survive a backend reorder,
  // while keyless rows keep the positional fallback. All stored ids
  // stay distinct and nonzero.
  const auto ingest = [this](const char *json) {
    monitor.ingestSnapshot(KWinBackend::parseSnapshot(json));
    return monitor.windows();
  };
  QList<WindowInfo> first = ingest(R"([
    {"id": 0, "key": "window/aaa", "title": "Game"},
    {"id": 0, "key": "window/bbb", "title": "Chat"},
    {"id": 0, "title": "NoKey"}])");
  ASSERT_EQ(first.size(), 3);
  QSet<quint64> ids;
  for (const WindowInfo &w : first) {
    EXPECT_NE(w.id, 0u);
    EXPECT_FALSE(ids.contains(w.id));
    ids.insert(w.id);
  }
  quint64 gameId = 0, chatId = 0;
  for (const WindowInfo &w : first) {
    if (w.title == "Game")
      gameId = w.id;
    if (w.title == "Chat")
      chatId = w.id;
  }
  ASSERT_NE(gameId, 0u);
  ASSERT_NE(chatId, 0u);

  // Same windows, reversed order: keyed ids must not move.
  QList<WindowInfo> second = ingest(R"([
    {"id": 0, "title": "NoKey"},
    {"id": 0, "key": "window/bbb", "title": "Chat"},
    {"id": 0, "key": "window/aaa", "title": "Game"}])");
  ASSERT_EQ(second.size(), 3);
  for (const WindowInfo &w : second) {
    if (w.title == "Game")
      EXPECT_EQ(w.id, gameId);
    if (w.title == "Chat")
      EXPECT_EQ(w.id, chatId);
  }
}

TEST_F(WindowMonitorTest, FocusChangeEmitsOncePerChange) {
  QSignalSpy focusSpy(&monitor, &WaylandWindowMonitor::focusedAppChanged);

  monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));
  EXPECT_EQ(focusSpy.count(), 1);

  // Same snapshot again: windowsChanged fires, focus does not.
  QSignalSpy windowsSpy(&monitor, &WaylandWindowMonitor::windowsChanged);
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));
  EXPECT_EQ(focusSpy.count(), 1);
  EXPECT_EQ(windowsSpy.count(), 1);

  // Focus moves to firefox.
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(R"([
    {"id": 101, "appId": "steam_app_730", "title": "Game",
     "pid": 4242, "fullscreen": false, "focused": false},
    {"id": 102, "appId": "firefox", "title": "Browser",
     "pid": 1234, "fullscreen": false, "focused": true}
  ])"));
  EXPECT_EQ(monitor.focusedAppId(), "firefox");
  EXPECT_EQ(focusSpy.count(), 2);
}

TEST_F(WindowMonitorTest, NoFocusedWindowClearsFocus) {
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));
  ASSERT_EQ(monitor.focusedAppId(), "steam_app_730");

  QSignalSpy focusSpy(&monitor, &WaylandWindowMonitor::focusedAppChanged);
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(R"([
    {"id": 101, "appId": "steam_app_730", "title": "Game",
     "pid": 4242, "fullscreen": false, "focused": false}
  ])"));
  EXPECT_TRUE(monitor.focusedAppId().isEmpty());
  EXPECT_EQ(focusSpy.count(), 1);
}

TEST_F(WindowMonitorTest, FullscreenFlagVisibleToConsumers) {
  monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));
  bool foundFullscreenGame = false;
  for (const WindowInfo &w : monitor.windows()) {
    if (w.focused && w.fullscreen && w.appId == "steam_app_730")
      foundFullscreenGame = true;
  }
  EXPECT_TRUE(foundFullscreenGame);
}

TEST_F(WindowMonitorTest, DetectCompositorCanonicalizes) {
  const QByteArray oldDesktop = qgetenv("XDG_CURRENT_DESKTOP");
  const QByteArray oldSession = qgetenv("DESKTOP_SESSION");
  const QByteArray oldHypr = qgetenv("HYPRLAND_INSTANCE_SIGNATURE");
  const QByteArray oldSway = qgetenv("SWAYSOCK");
  const QByteArray oldKde = qgetenv("KDE_SESSION_VERSION");

  // KDE sessions report KDE/Plasma, but the backend key is kwin.
  // (Regression: XDG_CURRENT_DESKTOP=KDE fell through to "unknown".)
  qputenv("HYPRLAND_INSTANCE_SIGNATURE", "");
  qputenv("SWAYSOCK", "");
  qputenv("KDE_SESSION_VERSION", "");

  qputenv("XDG_CURRENT_DESKTOP", "KDE");
  qputenv("DESKTOP_SESSION", "");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "kwin");

  qputenv("XDG_CURRENT_DESKTOP", "plasma");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "kwin");

  qputenv("XDG_CURRENT_DESKTOP", "GNOME");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "gnome");

  qputenv("XDG_CURRENT_DESKTOP", "Hyprland");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "hyprland");

  qputenv("XDG_CURRENT_DESKTOP", "sway");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "sway");

  // DESKTOP_SESSION fallback when XDG_CURRENT_DESKTOP is empty.
  qputenv("XDG_CURRENT_DESKTOP", "");
  qputenv("DESKTOP_SESSION", "plasma");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "kwin");

  // Nothing set at all.
  qputenv("DESKTOP_SESSION", "");
  EXPECT_EQ(WaylandWindowMonitor::detectCompositor(), "unknown");

  qputenv("XDG_CURRENT_DESKTOP", oldDesktop);
  qputenv("DESKTOP_SESSION", oldSession);
  qputenv("HYPRLAND_INSTANCE_SIGNATURE", oldHypr);
  qputenv("SWAYSOCK", oldSway);
  qputenv("KDE_SESSION_VERSION", oldKde);
}

TEST_F(WindowMonitorTest, CompositorKnownBeforeMonitoringStarts) {
  // Regression: the wizard showed an empty compositor name with tracking
  // opted out, because m_compositor was only set in startMonitoring().
  // Never start monitoring here; the constructor must have detected.
  WaylandWindowMonitor fresh;
  EXPECT_FALSE(fresh.isMonitoring());
  EXPECT_FALSE(fresh.compositor().isEmpty());
}

// --- WindowService delegation ---

class WindowServiceMonitorTest : public ::testing::Test {
protected:
  WaylandWindowMonitor monitor;
  WindowService service;

  void SetUp() override {
    service.setWindowMonitor(&monitor);
    monitor.ingestSnapshot(KWinBackend::parseSnapshot(kSnapshot));
  }
};

TEST_F(WindowServiceMonitorTest, VisibleWindowsExposePickerFields) {
  const QVariantList wins = service.getVisibleWindows();
  ASSERT_EQ(wins.size(), 2);
  EXPECT_EQ(wins[0].toMap()["title"].toString(), "Counter-Strike 2");
  EXPECT_EQ(wins[0].toMap()["class"].toString(), "steam_app_730");
  EXPECT_EQ(wins[0].toMap()["id"].toULongLong(), 101u);
}

TEST_F(WindowServiceMonitorTest, TitlePidClassLookupById) {
  EXPECT_EQ(service.getWindowTitle(101), "Counter-Strike 2");
  EXPECT_EQ(service.getPidForWindow(101), 4242u);
  EXPECT_EQ(service.getWindowClassForWindow(101), "steam_app_730");
}

TEST_F(WindowServiceMonitorTest, UnknownIdReturnsEmpty) {
  EXPECT_TRUE(service.getWindowTitle(9999).isEmpty());
  EXPECT_EQ(service.getPidForWindow(9999), 0u);
  EXPECT_TRUE(service.getWindowClassForWindow(9999).isEmpty());
}

TEST_F(WindowServiceMonitorTest, NoMonitorIsSafeNoOp) {
  WindowService bare;
  EXPECT_TRUE(bare.getVisibleWindows().isEmpty());
  EXPECT_TRUE(bare.getWindowTitle(101).isEmpty());
  EXPECT_EQ(bare.getPidForWindow(101), 0u);
}

// --- WindowInfo geometry ---

TEST(WindowInfoTest, FromJsonReadsGeometryWithDefaults) {
  const QJsonObject o = QJsonDocument::fromJson(
                            R"({"id": 3, "title": "G", "x": 0, "y": 0,
                                "width": 1920, "height": 1080,
                                "outputWidth": 1920, "outputHeight": 1080})")
                            .object();
  const WindowInfo w = WindowInfo::fromJson(o);
  EXPECT_EQ(w.width, 1920);
  EXPECT_EQ(w.height, 1080);
  EXPECT_EQ(w.outputWidth, 1920);
  EXPECT_EQ(w.outputHeight, 1080);

  const WindowInfo bare =
      WindowInfo::fromJson(QJsonDocument::fromJson(R"({"id": 4})").object());
  EXPECT_EQ(bare.width, 0);
  EXPECT_EQ(bare.outputWidth, 0);
}

// --- GnomeBackend::parseEvalResult ---

static const char *kGnomeEval = R"([
  {"id": 77001, "appId": "steam_app_1172470", "title": "Apex Legends",
   "pid": 9911, "fullscreen": true, "focused": true},
  {"id": 77002, "appId": "org.gnome.Terminal", "title": "Terminal",
   "pid": 2201, "fullscreen": false, "focused": false}
])";

TEST(GnomeBackendParseTest, ValidEvalResultParsesAllFields) {
  const QList<WindowInfo> wins = GnomeBackend::parseEvalResult(kGnomeEval);
  ASSERT_EQ(wins.size(), 2);
  EXPECT_EQ(wins[0].id, 77001u);
  EXPECT_EQ(wins[0].appId, "steam_app_1172470");
  EXPECT_EQ(wins[0].title, "Apex Legends");
  EXPECT_EQ(wins[0].pid, 9911);
  EXPECT_TRUE(wins[0].fullscreen);
  EXPECT_TRUE(wins[0].focused);
  EXPECT_FALSE(wins[1].fullscreen);
}

TEST(GnomeBackendParseTest, NullAndEmptyYieldEmptyList) {
  EXPECT_TRUE(GnomeBackend::parseEvalResult("null").isEmpty());
  EXPECT_TRUE(GnomeBackend::parseEvalResult("").isEmpty());
  EXPECT_TRUE(GnomeBackend::parseEvalResult("   ").isEmpty());
  EXPECT_TRUE(GnomeBackend::parseEvalResult("[]").isEmpty());
}

TEST(GnomeBackendParseTest, GarbageYieldsEmptyList) {
  EXPECT_TRUE(GnomeBackend::parseEvalResult("not json").isEmpty());
  EXPECT_TRUE(GnomeBackend::parseEvalResult("{oops}").isEmpty());
}

TEST(GnomeBackendParseTest, QuotedPayloadUnwrapped) {
  // Some Shell builds double-stringify the Eval result.
  const QString quoted =
      QString('"') + QString(kGnomeEval).replace("\"", "\\\"") + '"';
  const QList<WindowInfo> wins = GnomeBackend::parseEvalResult(quoted);
  ASSERT_EQ(wins.size(), 2);
  EXPECT_EQ(wins[0].title, "Apex Legends");
}

TEST(GnomeBackendParseTest, MissingFieldsDefaultSafely) {
  const QList<WindowInfo> wins =
      GnomeBackend::parseEvalResult(R"([{"id": 5, "title": "Bare"}])");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_TRUE(wins[0].appId.isEmpty());
  EXPECT_EQ(wins[0].pid, 0);
  EXPECT_FALSE(wins[0].focused);
}

TEST(GnomeBackendParseTest, SnapshotFeedsMonitorFocus) {
  WaylandWindowMonitor gnomeMonitor;
  QSignalSpy focusSpy(&gnomeMonitor,
                      &WaylandWindowMonitor::focusedAppChanged);
  gnomeMonitor.ingestSnapshot(GnomeBackend::parseEvalResult(kGnomeEval));
  EXPECT_EQ(gnomeMonitor.focusedAppId(), "steam_app_1172470");
  EXPECT_EQ(focusSpy.count(), 1);
}

TEST(GnomeBackendParseTest, SignatureStableAndSensitive) {
  const QList<WindowInfo> a = GnomeBackend::parseEvalResult(kGnomeEval);
  const QList<WindowInfo> b = GnomeBackend::parseEvalResult(kGnomeEval);
  EXPECT_EQ(GnomeBackend::signatureFor(a), GnomeBackend::signatureFor(b));

  QList<WindowInfo> changed = a;
  ASSERT_FALSE(changed.isEmpty());
  changed[0].focused = !changed[0].focused;
  EXPECT_NE(GnomeBackend::signatureFor(a), GnomeBackend::signatureFor(changed));
  changed = a;
  changed[0].width += 100;
  EXPECT_NE(GnomeBackend::signatureFor(a), GnomeBackend::signatureFor(changed))
      << "borderless coverage must refresh on resize";
  EXPECT_TRUE(GnomeBackend::signatureFor({}).isEmpty());
}

TEST(WindowInfoTest, StringEncodedIdsAndPidsAreParsed) {
  const auto w = WindowInfo::fromJson(QJsonObject{
      {"id", QStringLiteral("9007199254740993")},
      {"pid", QStringLiteral("4242")},
      {"appId", QStringLiteral("steam_app_1")}});
  EXPECT_EQ(w.id, 9007199254740993ULL);
  EXPECT_EQ(w.pid, 4242);
}

TEST(WindowInfoTest, MinimizedStateIsPreserved) {
  const auto windows = KWinBackend::parseSnapshot(
      R"([{"title":"Game","pid":4242,"fullscreen":true,"minimized":true}])");
  ASSERT_EQ(windows.size(), 1);
  EXPECT_TRUE(windows.first().minimized);
  EXPECT_FALSE(WindowInfo::fromJson(QJsonObject{}).minimized);
}

TEST(GnomeBackendParseTest, RefreshWithoutShellIsSilentNoOp) {
  if (QDBusConnection::sessionBus().interface()->isServiceRegistered(
          GnomeBackend::kService))
    GTEST_SKIP() << "real GNOME Shell present; polling it here is out of scope";
  GnomeBackend backend;
  QSignalSpy spy(&backend, &GnomeBackend::windowsUpdated);
  EXPECT_NO_THROW(backend.refresh());
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(backend.windows().isEmpty());
  EXPECT_FALSE(backend.isActive());
}

// --- HyprlandBackend::parseClients ---

static const char *kHyprClients = R"([
  {"address": "0x5c57d8e5d4c0", "class": "steam_app_1172470",
   "initialClass": "steam_app_1172470", "title": "Apex Legends",
   "initialTitle": "Apex Legends", "pid": 9911, "xwayland": false,
   "floating": false, "fullscreen": 2, "focusHistoryID": 3},
  {"address": "0x5c57d8e5d4d1", "class": "kitty", "initialClass": "kitty",
   "title": "kitty", "initialTitle": "kitty", "pid": 3451,
   "xwayland": false, "floating": false, "fullscreen": 0,
   "focusHistoryID": 0}
])";

static const char *kHyprActive = R"({"address": "0x5c57d8e5d4c0",
  "title": "Apex Legends", "class": "steam_app_1172470", "pid": 9911})";

TEST(HyprlandBackendParseTest, ValidClientsParseAllFields) {
  const QList<WindowInfo> wins =
      HyprlandBackend::parseClients(kHyprClients, "0x5c57d8e5d4c0");
  ASSERT_EQ(wins.size(), 2);
  EXPECT_EQ(wins[0].appId, "steam_app_1172470");
  EXPECT_EQ(wins[0].title, "Apex Legends");
  EXPECT_EQ(wins[0].pid, 9911);
  EXPECT_TRUE(wins[0].fullscreen); // mode 2
  EXPECT_TRUE(wins[0].focused);    // matches activewindow
  EXPECT_FALSE(wins[1].fullscreen);
  EXPECT_FALSE(wins[1].focused);
  EXPECT_NE(wins[0].id, 0u);
  EXPECT_NE(wins[0].id, wins[1].id);
}

TEST(HyprlandBackendParseTest, GarbageYieldsEmptyList) {
  EXPECT_TRUE(HyprlandBackend::parseClients("not json", "").isEmpty());
  EXPECT_TRUE(HyprlandBackend::parseClients("", "").isEmpty());
  EXPECT_TRUE(HyprlandBackend::parseClients("[]", "").isEmpty());
}

TEST(HyprlandBackendParseTest, FocusHistoryFallbackWithoutActiveWindow) {
  // No activewindow reply: focusHistoryID 0 (kitty) wins.
  const QList<WindowInfo> wins = HyprlandBackend::parseClients(kHyprClients, "");
  ASSERT_EQ(wins.size(), 2);
  EXPECT_FALSE(wins[0].focused);
  EXPECT_TRUE(wins[1].focused);
}

TEST(HyprlandBackendParseTest, MaximizedIsNotFullscreen) {
  const QList<WindowInfo> wins = HyprlandBackend::parseClients(
      R"([{"address": "0x1", "class": "game", "title": "Game",
           "pid": 7, "fullscreen": 1, "focusHistoryID": 0}])",
      "");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_FALSE(wins[0].fullscreen); // mode 1 == maximized
  EXPECT_TRUE(wins[0].focused);
}

TEST(HyprlandBackendParseTest, MissingClassFallsBackToInitial) {
  const QList<WindowInfo> wins = HyprlandBackend::parseClients(
      R"([{"address": "0x2", "class": "", "initialClass": "steam",
           "title": "", "initialTitle": "Steam", "pid": 9,
           "fullscreen": 0, "focusHistoryID": 5}])",
      "");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].appId, "steam");
  EXPECT_EQ(wins[0].title, "Steam");
}

static const char *kHyprMonitors = R"([
  {"id": 0, "width": 1920, "height": 1080,
   "activeWorkspace": {"id": 1, "name": "1"}},
  {"id": 1, "width": 2560, "height": 1440,
   "activeWorkspace": {"id": 2, "name": "2"}}
])";

TEST(HyprlandBackendParseTest, MonitorsProvideOutputSize) {
  const QList<WindowInfo> wins = HyprlandBackend::parseClients(
      R"([{"address": "0x3", "class": "game", "title": "Game", "pid": 11,
           "at": [0, 0], "size": [1920, 1080], "fullscreen": 0,
           "focusHistoryID": 0, "workspace": {"id": 1}}])",
      "", kHyprMonitors);
  ASSERT_EQ(wins.size(), 1);
  EXPECT_EQ(wins[0].x, 0);
  EXPECT_EQ(wins[0].y, 0);
  EXPECT_EQ(wins[0].width, 1920);
  EXPECT_EQ(wins[0].height, 1080);
  EXPECT_EQ(wins[0].outputWidth, 1920);
  EXPECT_EQ(wins[0].outputHeight, 1080);
}

TEST(HyprlandBackendParseTest, SnapshotFeedsMonitorFocus) {
  WaylandWindowMonitor hyprMonitor;
  QSignalSpy focusSpy(&hyprMonitor,
                      &WaylandWindowMonitor::focusedAppChanged);
  hyprMonitor.ingestSnapshot(HyprlandBackend::parseClients(kHyprClients,
                                                           "0x5c57d8e5d4c0"));
  EXPECT_EQ(hyprMonitor.focusedAppId(), "steam_app_1172470");
  EXPECT_EQ(focusSpy.count(), 1);
}

TEST(HyprlandBackendParseTest, RefreshWithoutHyprlandIsSilentNoOp) {
  if (!QStandardPaths::findExecutable("hyprctl").isEmpty())
    GTEST_SKIP() << "hyprctl present; polling it here is out of scope";
  HyprlandBackend backend;
  QSignalSpy spy(&backend, &HyprlandBackend::windowsUpdated);
  EXPECT_NO_THROW(backend.refresh());
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(backend.windows().isEmpty());
}

// --- SwayBackend::parseTree ---

static const char *kSwayTree = R"({
  "id": 1, "name": "root", "type": "root",
  "nodes": [{
    "id": 2, "name": "HDMI-1", "type": "output",
    "rect": {"x": 0, "y": 0, "width": 1920, "height": 1080},
    "nodes": [{
      "id": 3, "name": "1", "type": "workspace",
      "nodes": [
        {"id": 10, "name": "Hades II", "type": "con",
         "rect": {"x": 0, "y": 0, "width": 1920, "height": 1080},
         "focused": true, "fullscreen_mode": 1, "pid": 5511,
         "app_id": "steam_app_1145350", "visible": true, "nodes": [],
         "floating_nodes": []},
        {"id": 11, "name": "urxvt", "type": "con",
         "rect": {"x": 0, "y": 0, "width": 960, "height": 1057},
         "focused": false, "fullscreen_mode": 0, "pid": 23959,
         "app_id": null, "visible": true, "nodes": [], "floating_nodes": [],
         "window_properties": {"class": "URxvt", "instance": "urxvt",
                               "title": "urxvt"}}
      ],
      "floating_nodes": [
        {"id": 12, "name": "Volume", "type": "floating_con",
         "focused": false, "fullscreen_mode": 0, "pid": 777,
         "app_id": "pavucontrol", "visible": true, "nodes": [],
         "floating_nodes": []}
      ]
    }]
  }]
})";

TEST(SwayBackendParseTest, TreeWalkFindsTiledAndFloating) {
  const QList<WindowInfo> wins = SwayBackend::parseTree(kSwayTree);
  ASSERT_EQ(wins.size(), 3);

  EXPECT_EQ(wins[0].id, 10u);
  EXPECT_EQ(wins[0].appId, "steam_app_1145350");
  EXPECT_EQ(wins[0].title, "Hades II");
  EXPECT_EQ(wins[0].pid, 5511);
  EXPECT_TRUE(wins[0].fullscreen);
  EXPECT_TRUE(wins[0].focused);
  EXPECT_EQ(wins[0].width, 1920);
  EXPECT_EQ(wins[0].height, 1080);
  EXPECT_EQ(wins[0].outputWidth, 1920);
  EXPECT_EQ(wins[0].outputHeight, 1080);

  // XWayland fallback to window class.
  EXPECT_EQ(wins[1].appId, "URxvt");
  EXPECT_EQ(wins[1].title, "urxvt");
  EXPECT_FALSE(wins[1].focused);

  // Floating node collected too.
  EXPECT_EQ(wins[2].id, 12u);
  EXPECT_EQ(wins[2].appId, "pavucontrol");
}

TEST(SwayBackendParseTest, GarbageYieldsEmptyList) {
  EXPECT_TRUE(SwayBackend::parseTree("not json").isEmpty());
  EXPECT_TRUE(SwayBackend::parseTree("").isEmpty());
}

TEST(SwayBackendParseTest, GlobalFullscreenCounts) {
  const QList<WindowInfo> wins = SwayBackend::parseTree(
      R"({"id": 1, "type": "root", "nodes": [],
          "floating_nodes": [{"id": 9, "name": "Game", "type": "con",
          "focused": true, "fullscreen_mode": 2, "pid": 5,
          "app_id": "game", "nodes": [], "floating_nodes": []}]})");
  ASSERT_EQ(wins.size(), 1);
  EXPECT_TRUE(wins[0].fullscreen);
}

TEST(SwayBackendParseTest, SnapshotFeedsMonitorFocus) {
  WaylandWindowMonitor swayMonitor;
  QSignalSpy focusSpy(&swayMonitor,
                      &WaylandWindowMonitor::focusedAppChanged);
  swayMonitor.ingestSnapshot(SwayBackend::parseTree(kSwayTree));
  EXPECT_EQ(swayMonitor.focusedAppId(), "steam_app_1145350");
  EXPECT_EQ(focusSpy.count(), 1);
}

TEST(SwayBackendParseTest, RefreshWithoutSwayIsSilentNoOp) {
  if (!QStandardPaths::findExecutable("swaymsg").isEmpty())
    GTEST_SKIP() << "swaymsg present; polling it here is out of scope";
  SwayBackend backend;
  QSignalSpy spy(&backend, &SwayBackend::windowsUpdated);
  EXPECT_NO_THROW(backend.refresh());
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(backend.windows().isEmpty());
}

// --- KWin script packaging (hermetic: fake package + temp XDG dirs) ---

class KWinScriptInstallTest : public ::testing::Test {
protected:
  QTemporaryDir dataHome;
  QTemporaryDir packageDir;
  QByteArray oldXdgDataHome;
  QByteArray oldOverride;

  void SetUp() override {
    ASSERT_TRUE(dataHome.isValid());
    ASSERT_TRUE(packageDir.isValid());
    // The enable cache is static: reset per test so a future test that
    // runs the setup worker can't flip EnabledFalseWithoutKwinTools
    // order-dependently. Unknown (-1) reads as disabled.
    KWinBackend::s_enabledCache.store(-1, std::memory_order_release);

    // Build a minimal fake script package.
    QDir().mkpath(packageDir.path() + "/contents/code");
    QFile meta(packageDir.path() + "/metadata.json");
    ASSERT_TRUE(meta.open(QIODevice::WriteOnly));
    meta.write("{\"KPlugin\": {\"Id\": \"trophy-monitor\"}}");
    meta.close();
    QFile script(packageDir.path() + "/contents/code/main.js");
    ASSERT_TRUE(script.open(QIODevice::WriteOnly));
    script.write("// fake trophy monitor");
    script.close();

    oldXdgDataHome = qgetenv("XDG_DATA_HOME");
    oldOverride = qgetenv("TROPHY_KWIN_SCRIPT_DIR");
    qputenv("XDG_DATA_HOME", dataHome.path().toUtf8());
    qputenv("TROPHY_KWIN_SCRIPT_DIR", packageDir.path().toUtf8());
  }

  void TearDown() override {
    if (oldXdgDataHome.isNull())
      qunsetenv("XDG_DATA_HOME");
    else
      qputenv("XDG_DATA_HOME", oldXdgDataHome);
    if (oldOverride.isNull())
      qunsetenv("TROPHY_KWIN_SCRIPT_DIR");
    else
      qputenv("TROPHY_KWIN_SCRIPT_DIR", oldOverride);
  }
};

TEST_F(KWinScriptInstallTest, InstallRemoveRoundTrip) {
  ASSERT_FALSE(KWinBackend::isScriptInstalled());

  KWinBackend backend;
  ASSERT_TRUE(backend.ensureScriptInstalled());
  EXPECT_TRUE(KWinBackend::isScriptInstalled());

  // Installed content matches the source package.
  QFile installed(KWinBackend::installedScriptDir() +
                  "/contents/code/main.js");
  ASSERT_TRUE(installed.open(QIODevice::ReadOnly));
  EXPECT_EQ(installed.readAll(), QByteArray("// fake trophy monitor"));

  // Second run is a no-op success (no KWin reload needed).
  EXPECT_TRUE(backend.ensureScriptInstalled());

  EXPECT_TRUE(KWinBackend::removeInstalledScript());
  EXPECT_FALSE(KWinBackend::isScriptInstalled());
}

TEST_F(KWinScriptInstallTest, RemoveWhenAbsentSucceeds) {
  EXPECT_TRUE(KWinBackend::removeInstalledScript());
}

TEST_F(KWinScriptInstallTest, FindsBundledPackageUnderInstallPrefix) {
  // AppImage-style layout: binary in <prefix>/bin, package shipped at
  // <prefix>/share/trophy/kwin/trophy-monitor. Previously only the
  // shared datadir and the dev tree were consulted, so the bundled
  // copy was invisible and enabling tracking silently did nothing.
  QTemporaryDir prefix;
  ASSERT_TRUE(prefix.isValid());
  ASSERT_TRUE(QDir().mkpath(prefix.path() + "/bin"));
  ASSERT_TRUE(
      QDir().mkpath(prefix.path() + "/share/trophy/kwin/trophy-monitor"));
  QFile meta(prefix.path() +
             "/share/trophy/kwin/trophy-monitor/metadata.json");
  ASSERT_TRUE(meta.open(QIODevice::WriteOnly));
  meta.write("{\"KPlugin\": {\"Id\": \"trophy-monitor\"}}");
  meta.close();

  EXPECT_EQ(KWinBackend::findScriptSourceDirForAppDir(prefix.path() + "/bin"),
            prefix.path() + "/share/trophy/kwin/trophy-monitor");
}

TEST_F(KWinScriptInstallTest, EnabledFalseWithoutKwinTools) {
  if (!QStandardPaths::findExecutable("kreadconfig6").isEmpty() ||
      !QStandardPaths::findExecutable("kreadconfig5").isEmpty())
    GTEST_SKIP() << "KDE tools present; result depends on real kwinrc";
  EXPECT_FALSE(KWinBackend::isScriptEnabled());
}
