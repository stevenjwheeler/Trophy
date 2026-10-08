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
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>

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
  EXPECT_TRUE(GnomeBackend::signatureFor({}).isEmpty());
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

TEST_F(KWinScriptInstallTest, EnabledFalseWithoutKwinTools) {
  if (!QStandardPaths::findExecutable("kreadconfig6").isEmpty() ||
      !QStandardPaths::findExecutable("kreadconfig5").isEmpty())
    GTEST_SKIP() << "KDE tools present; result depends on real kwinrc";
  EXPECT_FALSE(KWinBackend::isScriptEnabled());
}
