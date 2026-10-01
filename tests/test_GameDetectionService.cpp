#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <chrono>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>

#include "detection/GameDetectionService.h"
#include "detection/WaylandWindowMonitor.h"
#include "detection/WindowInfo.h"

class GameDetectionTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new GameDetectionTestEnvironment);

namespace TrophyBackend {

class GameDetectionServiceTest : public ::testing::Test {
protected:
  // NOTE: declared first so it outlives the service.
  QTemporaryDir configHome;
  QByteArray oldXdgConfigHome;
  WaylandWindowMonitor monitor;
  GameDetectionService service;

  void SetUp() override {
    ASSERT_TRUE(configHome.isValid());
    // GameDetectionService persists whitelist/blacklist to
    // XDG_CONFIG_HOME/trophy — keep tests off the user's real config.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    service.setWindowMonitor(&monitor);
    service.start();
  }

  void TearDown() override {
    service.stop();
    if (oldXdgConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);
  }

  // A held connection keeps the service's per-PID state alive: closing
  // the socket right after START makes the server process a disconnect
  // and immediately retire the game again.
  class GameClient {
  public:
    bool connect() {
      socket.connectToServer("/tmp/trophy-detection.sock");
      return socket.waitForConnected(1000);
    }
    bool send(const QString &line) {
      socket.write(line.toUtf8());
      return socket.waitForBytesWritten(1000);
    }
    QLocalSocket socket;
  };

  bool sendLine(const QString &line) {
    GameClient client;
    if (!client.connect())
      return false;
    return client.send(line);
  }

  void pump(int ms = 500) {
    const int step = 25;
    for (int waited = 0; waited < ms; waited += step) {
      QCoreApplication::processEvents();
      QThread::msleep(step);
    }
    QCoreApplication::processEvents();
  }

  // waitForStarted fires at fork; /proc/PID/environ fills in at exec.
  // Poll until the child image is running (or timeout).
  static bool waitForChildExec(qint64 pid, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      QFile cmd(QString("/proc/%1/cmdline").arg(pid));
      if (cmd.open(QIODevice::ReadOnly) && !cmd.readAll().isEmpty())
        return true;
      QThread::msleep(10);
    }
    return false;
  }

  void simulateGameStart(qint64 pid, const QString &name) {
    QLocalSocket socket;
    socket.connectToServer("/tmp/trophy-detection.sock");
    if (socket.waitForConnected(1000)) {
        socket.write(QString("START:%1:%2\n").arg(pid).arg(name).toUtf8());
        socket.waitForBytesWritten(1000);
        // We keep the socket alive if we want to simulate a running game
        // But for a simple test, we can just wait for the service to process it
        QCoreApplication::processEvents();
    }
  }
};

TEST_F(GameDetectionServiceTest, GameDetectionViaSocket) {
  // Whitelisted reporters detect instantly without window state.
  service.addToWhitelist("Test Game", "Test Game");
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  QLocalSocket socket;
  socket.connectToServer("/tmp/trophy-detection.sock");
  ASSERT_TRUE(socket.waitForConnected(1000));

  socket.write("START:1234:Test Game\n");
  socket.waitForBytesWritten(1000);
  
  // Give the service time to process the message
  int retries = 0;
  while (spy.count() == 0 && retries < 10) {
      QCoreApplication::processEvents();
      QThread::msleep(50);
      retries++;
  }

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Test Game");
  EXPECT_EQ(service.currentGamePid(), 1234);
}

TEST_F(GameDetectionServiceTest, GameClosedOnDisconnect) {
  service.addToWhitelist("Test Game", "Test Game");
  QSignalSpy spy(&service, &GameDetectionService::gameClosed);
  
  {
      QLocalSocket socket;
      socket.connectToServer("/tmp/trophy-detection.sock");
      ASSERT_TRUE(socket.waitForConnected(1000));
      socket.write("START:1234:Test Game\n");
      socket.waitForBytesWritten(1000);
      
      QCoreApplication::processEvents();
      QThread::msleep(100);
      QCoreApplication::processEvents();
      
      ASSERT_EQ(service.currentGamePid(), 1234);
      
      // Socket goes out of scope and disconnects
  }
  
  int retries = 0;
  while (spy.count() == 0 && retries < 10) {
      QCoreApplication::processEvents();
      QThread::msleep(50);
      retries++;
  }

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, WhitelistManagement) {
  QString windowClass = "steam_app_test";
  QString displayName = "Test Game";

  service.addToWhitelist(windowClass, displayName);

  QStringList whitelist = service.whitelist();
  EXPECT_TRUE(whitelist.contains(windowClass));

  service.removeFromWhitelist(windowClass);
  whitelist = service.whitelist();
  EXPECT_FALSE(whitelist.contains(windowClass));
}

TEST_F(GameDetectionServiceTest, BlacklistedProcessIgnored) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  ASSERT_TRUE(sendLine("START:9999:discord\n"));
  pump();

  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());
  EXPECT_EQ(service.currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, StopMessageClosesGame) {
  service.addToWhitelist("Some Game", "Some Game");
  QSignalSpy closedSpy(&service, &GameDetectionService::gameClosed);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:4321:Some Game\n"));
  pump(300);
  ASSERT_EQ(service.currentGamePid(), 4321);

  // STOP on the same connection mirrors trophy_layer's DestroyInstance
  // (send STOP, then close). A STOP from an unrelated connection must not
  // retire a still-connected game.
  ASSERT_TRUE(game.send("STOP:4321\n"));
  game.socket.disconnectFromServer();
  game.socket.waitForDisconnected(1000);
  pump();

  EXPECT_EQ(closedSpy.count(), 1);
  EXPECT_TRUE(service.currentGameName().isEmpty());
  EXPECT_EQ(service.currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, GenericFallbackNameIgnored) {
  service.addToWhitelist("Real Game", "Real Game");
  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:5555:Real Game\n"));
  pump(300);
  ASSERT_EQ(service.currentGameName(), "Real Game");

  // The Vulkan layer's generic fallback is blacklisted, so it must neither
  // clobber the specific name nor create a second detection.
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);
  ASSERT_TRUE(sendLine("START:5555:Unknown Vulkan Game:0\n"));
  pump(300);
  EXPECT_EQ(service.currentGameName(), "Real Game");
  EXPECT_EQ(spy.count(), 0);
}

// --- Confidence fusion with the window monitor ---

static QList<WindowInfo> snapshotFor(qint64 pid, bool focused,
                                     bool fullscreen) {
  WindowInfo w;
  w.id = 9000 + (quint64)pid;
  w.appId = "some-game";
  w.title = "Some Game";
  w.pid = pid;
  w.focused = focused;
  w.fullscreen = fullscreen;
  return {w};
}

TEST_F(GameDetectionServiceTest, SteamAppIdDetectsInstantly) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  // Proton-style report: no whitelist, no window state, Steam id present.
  ASSERT_TRUE(game.send("START:6101:Proton Game:1172470\n"));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Proton Game");
  EXPECT_EQ(service.currentGamePid(), 6101);
}

TEST_F(GameDetectionServiceTest, FocusedFullscreenDetectsWithoutWhitelist) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6201:Some Game\n"));
  pump(300);
  EXPECT_EQ(spy.count(), 0); // not confident yet: no whitelist/Steam

  monitor.ingestSnapshot(snapshotFor(6201, true, true));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Some Game");
  EXPECT_EQ(service.currentGamePid(), 6201);
}

TEST_F(GameDetectionServiceTest, FocusedWindowedStaysPending) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6301:Some Game\n"));
  pump(300);

  // Focused but windowed: still not a game.
  monitor.ingestSnapshot(snapshotFor(6301, true, false));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());

  // Same game goes fullscreen later: pending candidate promotes.
  monitor.ingestSnapshot(snapshotFor(6301, true, true));
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGamePid(), 6301);
}

TEST_F(GameDetectionServiceTest, UnfocusedFullscreenStaysPending) {  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6401:Some Game\n"));
  pump(300);

  // Fullscreen but not focused (e.g. background window): ignore.
  monitor.ingestSnapshot(snapshotFor(6401, false, true));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());
}

// --- Borderless-windowed coverage (no fullscreen flag) ---

static QList<WindowInfo> snapshotSized(qint64 pid, int w, int h, int ow,
                                       int oh) {
  WindowInfo win;
  win.id = 9500 + (quint64)pid;
  win.appId = "borderless-game";
  win.title = "Borderless Game";
  win.pid = pid;
  win.focused = true;
  win.fullscreen = false;
  win.width = w;
  win.height = h;
  win.outputWidth = ow;
  win.outputHeight = oh;
  return {win};
}

TEST_F(GameDetectionServiceTest, BorderlessCoveringOutputDetects) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6501:Borderless Game\n"));
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  // Focused 1920x1080 on a 1920x1080 output, no fullscreen flag.
  monitor.ingestSnapshot(snapshotSized(6501, 1920, 1080, 1920, 1080));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGamePid(), 6501);
}

TEST_F(GameDetectionServiceTest, FocusedSmallWindowStaysPending) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6601:Borderless Game\n"));
  pump(300);

  // Focused 800x600 on 1920x1080: not coverage, stays pending.
  monitor.ingestSnapshot(snapshotSized(6601, 800, 600, 1920, 1080));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, UnknownGeometryStillNeedsFlag) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6701:Borderless Game\n"));
  pump(300);

  // Backend reported no geometry: coverage rule cannot apply.
  monitor.ingestSnapshot(snapshotSized(6701, 0, 0, 0, 0));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());
}


// --- Startup reconciliation (already-running games) ---

static QList<WindowInfo> runningGameSnapshot() {
  WindowInfo w;
  w.id = 4242;
  w.appId = "reconciled-game";
  w.title = "Reconciled Game";
  w.pid = QCoreApplication::applicationPid(); // real PID: exe is NOT steam
  w.focused = true;
  w.fullscreen = true;
  return {w};
}

TEST_F(GameDetectionServiceTest, ReconcileFindsWhitelistedRunningGame) {
  service.addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  // No socket traffic at all: the game predates Trophy.
  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Reconciled Game");
  EXPECT_EQ(service.currentGamePid(), QCoreApplication::applicationPid());
}

TEST_F(GameDetectionServiceTest, ReconcileIgnoresUnlistedRunningGame) {
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  // Focused + fullscreen, but nothing links it to a game: no guess.
  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);

  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service.currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, ReconcileRunsOnceAutomatically) {
  service.addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  ASSERT_EQ(spy.count(), 1);

  // Second snapshot must not re-emit for the same game.
  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  EXPECT_EQ(spy.count(), 1);
}

TEST_F(GameDetectionServiceTest, ReconcileRetriesAfterEmptySnapshot) {
  // Regression: the first emission can be empty (startMonitoring re-emits
  // before the backend's first real snapshot). That must not consume the
  // one reconcile pass, or an already-running game is missed forever.
  service.addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  monitor.ingestSnapshot({});
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Reconciled Game");
}

TEST_F(GameDetectionServiceTest, ReconcileRetriesWhenUnfocused) {
  // Same for focus: a game that is backgrounded at startup must still be
  // picked up once it gains focus (focused/fullscreen gate still applies).
  service.addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(&service, &GameDetectionService::gameDetected);

  auto backgrounded = runningGameSnapshot();
  backgrounded[0].focused = false;
  backgrounded[0].fullscreen = false;
  monitor.ingestSnapshot(backgrounded);
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Reconciled Game");
}


TEST_F(GameDetectionServiceTest, SteamAppIdForPidReadsChildEnviron) {
  // NOTE: qputenv cannot change /proc/self/environ (fixed at exec), so
  // spawn a child with the variable instead. waitForStarted fires at
  // fork, before exec populates /proc/PID/environ — poll for it.
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("SteamAppId", "424242");
  QProcess child;
  child.setProcessEnvironment(env);
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(waitForChildExec(pid));

  EXPECT_EQ(GameDetectionService::steamAppIdForPid(pid), 424242u);

  child.kill();
  child.waitForFinished(5000);
  EXPECT_EQ(GameDetectionService::steamAppIdForPid(-1), 0u);
  EXPECT_EQ(GameDetectionService::steamAppIdForPid(0), 0u);
}

TEST_F(GameDetectionServiceTest, ReconcileFindsSteamEnvRunningGame) {
  // No whitelist: the SteamAppId in the process environment (as Proton
  // leaves it) is enough evidence, e.g. Trophy opened mid-game.
  // Hermetic HOME: this dev machine really has a 1422450 manifest, which
  // would resolve the display name and break the expectation below.
  QTemporaryDir emptyHome;
  ASSERT_TRUE(emptyHome.isValid());
  const QByteArray savedHome = qgetenv("HOME");
  qputenv("HOME", emptyHome.path().toUtf8());

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("SteamAppId", "1422450");
  QProcess child;
  child.setProcessEnvironment(env);
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(waitForChildExec(pid));

  QSignalSpy spy(&service, &GameDetectionService::gameDetected);
  WindowInfo w;
  w.id = 4243;
  w.appId = "reconciled-game";
  w.title = "Reconciled Game";
  w.pid = pid;
  w.focused = true;
  w.fullscreen = true;
  monitor.ingestSnapshot({w});
  pump(300);

  child.kill();
  child.waitForFinished(5000);
  if (savedHome.isNull())
    qunsetenv("HOME");
  else
    qputenv("HOME", savedHome);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "reconciled-game");
  EXPECT_EQ(service.currentGamePid(), pid);
}


TEST_F(GameDetectionServiceTest, SteamAppIdFromAppIdParses) {
  EXPECT_EQ(GameDetectionService::steamAppIdFromAppId("steam_app_1422450"),
            1422450u);
  EXPECT_EQ(GameDetectionService::steamAppIdFromAppId("STEAM_APP_730"), 730u);
  EXPECT_EQ(GameDetectionService::steamAppIdFromAppId("deadlock"), 0u);
  EXPECT_EQ(GameDetectionService::steamAppIdFromAppId(""), 0u);
  EXPECT_EQ(GameDetectionService::steamAppIdFromAppId("steam_app_"), 0u);
}

TEST_F(GameDetectionServiceTest, ReconcileResolvesSteamClassToDisplayName) {
  // Fake a Steam library: manifest lookup must turn the window class
  // steam_app_1422450 into "Deadlock", as on a real machine.
  QTemporaryDir fakeHome;
  ASSERT_TRUE(fakeHome.isValid());
  const QString appsDir = fakeHome.path() + "/.local/share/Steam/steamapps";
  ASSERT_TRUE(QDir().mkpath(appsDir));
  QFile vdf(appsDir + "/libraryfolders.vdf");
  ASSERT_TRUE(vdf.open(QIODevice::WriteOnly | QIODevice::Text));
  vdf.write("\"libraryfolders\"\n{\n}\n");
  vdf.close();
  QFile acf(appsDir + "/appmanifest_1422450.acf");
  ASSERT_TRUE(acf.open(QIODevice::WriteOnly | QIODevice::Text));
  acf.write("\"AppState\"\n{\n\t\"appid\"\t\t\"1422450\"\n"
            "\t\"name\"\t\t\"Deadlock\"\n}\n");
  acf.close();
  const QByteArray savedHome = qgetenv("HOME");
  qputenv("HOME", fakeHome.path().toUtf8());

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("SteamAppId", "1422450");
  QProcess child;
  child.setProcessEnvironment(env);
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  ASSERT_GT(pid, 0);
  ASSERT_TRUE(waitForChildExec(pid));

  QSignalSpy spy(&service, &GameDetectionService::gameDetected);
  WindowInfo w;
  w.id = 4244;
  w.appId = "steam_app_1422450";
  w.title = "steam_app_1422450";
  w.pid = pid;
  w.focused = true;
  w.fullscreen = true;
  monitor.ingestSnapshot({w});
  pump(300);

  child.kill();
  child.waitForFinished(5000);
  if (savedHome.isNull())
    qunsetenv("HOME");
  else
    qputenv("HOME", savedHome);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service.currentGameName(), "Deadlock");
  EXPECT_EQ(service.currentGamePid(), pid);
}

} // namespace TrophyBackend
