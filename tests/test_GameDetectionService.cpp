#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>

#include "detection/GameDetectionService.h"
#include "detection/GameListModel.h"
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

class FakeSteamHome {
public:
  FakeSteamHome(uint appId, const QString &name)
      : m_savedHome(qgetenv("HOME")) {
    const QString appsDir = m_home.path() + "/.local/share/Steam/steamapps";
    m_valid = m_home.isValid() && QDir().mkpath(appsDir);
    if (!m_valid)
      return;
    QFile libraries(appsDir + "/libraryfolders.vdf");
    QFile manifest(appsDir + QString("/appmanifest_%1.acf").arg(appId));
    m_valid = libraries.open(QIODevice::WriteOnly | QIODevice::Text) &&
              manifest.open(QIODevice::WriteOnly | QIODevice::Text);
    if (!m_valid)
      return;
    libraries.write("\"libraryfolders\" { }\n");
    manifest.write(
        QString("\"AppState\" { \"appid\" \"%1\" \"name\" \"%2\" }\n")
            .arg(appId)
            .arg(name)
            .toUtf8());
    qputenv("HOME", m_home.path().toUtf8());
  }

  ~FakeSteamHome() {
    if (m_savedHome.isNull())
      qunsetenv("HOME");
    else
      qputenv("HOME", m_savedHome);
  }

  bool isValid() const { return m_valid; }

private:
  QTemporaryDir m_home;
  QByteArray m_savedHome;
  bool m_valid = false;
};

namespace TrophyBackend {

class GameDetectionServiceTest : public ::testing::Test {
protected:
  // NOTE: declared first so they outlive the service.
  QTemporaryDir configHome;
  QByteArray oldXdgConfigHome;
  WaylandWindowMonitor monitor;
  // Heap-owned so construction happens AFTER XDG_CONFIG_HOME is
  // redirected below: the ctor loads whitelist/blacklist from disk, and
  // member-init order would otherwise read the user's real config.
  std::unique_ptr<GameDetectionService> service;

  void SetUp() override {
    ASSERT_TRUE(configHome.isValid());
    // GameDetectionService persists whitelist/blacklist to
    // XDG_CONFIG_HOME/trophy — keep tests off the user's real config.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    service = std::make_unique<GameDetectionService>();
    service->setWindowMonitor(&monitor);
    service->start();
  }

  void TearDown() override {
    service->stop();
    service.reset();
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
  service->addToWhitelist("Test Game", "Test Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

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
  EXPECT_EQ(service->currentGameName(), "Test Game");
  EXPECT_EQ(service->currentGamePid(), 1234);
}

TEST_F(GameDetectionServiceTest, GameClosedOnDisconnect) {
  service->addToWhitelist("Test Game", "Test Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameClosed);

  {
    QLocalSocket socket;
    socket.connectToServer("/tmp/trophy-detection.sock");
    ASSERT_TRUE(socket.waitForConnected(1000));
    socket.write("START:1234:Test Game\n");
    socket.waitForBytesWritten(1000);

    QCoreApplication::processEvents();
    QThread::msleep(100);
    QCoreApplication::processEvents();

    ASSERT_EQ(service->currentGamePid(), 1234);

    // Socket goes out of scope and disconnects
  }

  int retries = 0;
  while (spy.count() == 0 && retries < 10) {
    QCoreApplication::processEvents();
    QThread::msleep(50);
    retries++;
  }

  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, WhitelistManagement) {
  QString windowClass = "steam_app_test";
  QString displayName = "Test Game";

  service->addToWhitelist(windowClass, displayName);

  QStringList whitelist = service->whitelist();
  EXPECT_TRUE(whitelist.contains(windowClass));

  service->removeFromWhitelist(windowClass);
  whitelist = service->whitelist();
  EXPECT_FALSE(whitelist.contains(windowClass));
}

TEST_F(GameDetectionServiceTest, BlacklistedProcessIgnored) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  ASSERT_TRUE(sendLine("START:9999:discord\n"));
  pump();

  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, StopMessageClosesGame) {
  service->addToWhitelist("Some Game", "Some Game");
  QSignalSpy closedSpy(service.get(), &GameDetectionService::gameClosed);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:4321:Some Game\n"));
  pump(300);
  ASSERT_EQ(service->currentGamePid(), 4321);

  // STOP on the same connection mirrors trophy_layer's DestroyInstance
  // (send STOP, then close). A STOP from an unrelated connection must not
  // retire a still-connected game.
  ASSERT_TRUE(game.send("STOP:4321\n"));
  game.socket.disconnectFromServer();
  game.socket.waitForDisconnected(1000);
  pump();

  EXPECT_EQ(closedSpy.count(), 1);
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, GenericFallbackNameIgnored) {
  service->addToWhitelist("Real Game", "Real Game");
  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:5555:Real Game\n"));
  pump(300);
  ASSERT_EQ(service->currentGameName(), "Real Game");

  // The Vulkan layer's generic fallback is blacklisted, so it must neither
  // clobber the specific name nor create a second detection.
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);
  ASSERT_TRUE(sendLine("START:5555:Unknown Vulkan Game:0\n"));
  pump(300);
  EXPECT_EQ(service->currentGameName(), "Real Game");
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
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  // Proton-style report: no whitelist, no window state, Steam id present.
  ASSERT_TRUE(game.send("START:6101:Proton Game:1172470\n"));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGameName(), "Proton Game");
  EXPECT_EQ(service->currentGamePid(), 6101);
}

TEST_F(GameDetectionServiceTest, FocusedFullscreenDetectsWithoutWhitelist) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6201:Some Game\n"));
  pump(300);
  EXPECT_EQ(spy.count(), 0); // not confident yet: no whitelist/Steam

  monitor.ingestSnapshot(snapshotFor(6201, true, true));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGameName(), "Some Game");
  EXPECT_EQ(service->currentGamePid(), 6201);
}

TEST_F(GameDetectionServiceTest, FocusedWindowedStaysPending) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6301:Some Game\n"));
  pump(300);

  // Focused but windowed: still not a game.
  monitor.ingestSnapshot(snapshotFor(6301, true, false));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());

  // Same game goes fullscreen later: pending candidate promotes.
  monitor.ingestSnapshot(snapshotFor(6301, true, true));
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGamePid(), 6301);
}

TEST_F(GameDetectionServiceTest, UnfocusedFullscreenStaysPending) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6401:Some Game\n"));
  pump(300);

  // Fullscreen but not focused (e.g. background window): ignore.
  monitor.ingestSnapshot(snapshotFor(6401, false, true));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());
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
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6501:Borderless Game\n"));
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  // Focused 1920x1080 on a 1920x1080 output, no fullscreen flag.
  monitor.ingestSnapshot(snapshotSized(6501, 1920, 1080, 1920, 1080));
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGamePid(), 6501);
}

TEST_F(GameDetectionServiceTest, FocusedSmallWindowStaysPending) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6601:Borderless Game\n"));
  pump(300);

  // Focused 800x600 on 1920x1080: not coverage, stays pending.
  monitor.ingestSnapshot(snapshotSized(6601, 800, 600, 1920, 1080));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, UnknownGeometryStillNeedsFlag) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6701:Borderless Game\n"));
  pump(300);

  // Backend reported no geometry: coverage rule cannot apply.
  monitor.ingestSnapshot(snapshotSized(6701, 0, 0, 0, 0));
  pump(300);
  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());
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
  service->addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  // No socket traffic at all: the game predates Trophy.
  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);

  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGameName(), "Reconciled Game");
  EXPECT_EQ(service->currentGamePid(), QCoreApplication::applicationPid());
}

TEST_F(GameDetectionServiceTest, ReconcileIgnoresUnlistedRunningGame) {
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  // Focused + fullscreen, but nothing links it to a game: no guess.
  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);

  EXPECT_EQ(spy.count(), 0);
  EXPECT_TRUE(service->currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, ReconcileRunsOnceAutomatically) {
  service->addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

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
  service->addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  monitor.ingestSnapshot({});
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGameName(), "Reconciled Game");
}

TEST_F(GameDetectionServiceTest, ReconcileRetriesWhenUnfocused) {
  // Same for focus: a game that is backgrounded at startup must still be
  // picked up once it gains focus (focused/fullscreen gate still applies).
  service->addToWhitelist("reconciled-game", "Reconciled Game");
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);

  auto backgrounded = runningGameSnapshot();
  backgrounded[0].focused = false;
  backgrounded[0].fullscreen = false;
  monitor.ingestSnapshot(backgrounded);
  pump(300);
  EXPECT_EQ(spy.count(), 0);

  monitor.ingestSnapshot(runningGameSnapshot());
  pump(300);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(service->currentGameName(), "Reconciled Game");
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

  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);
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
  EXPECT_EQ(service->currentGameName(), "reconciled-game");
  EXPECT_EQ(service->currentGamePid(), pid);
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

  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);
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
  EXPECT_EQ(service->currentGameName(), "Deadlock");
  EXPECT_EQ(service->currentGamePid(), pid);
}

TEST_F(GameDetectionServiceTest, SocketStartHonorsBlacklistOnRawReporterKey) {
  constexpr uint appId = 1422451;
  FakeSteamHome steam(appId, "Display Blocked Game");
  ASSERT_TRUE(steam.isValid());
  ASSERT_TRUE(service->addToBlacklist("raw-blocked.exe"));

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send(QString("START:7241:raw-blocked.exe:%1\n").arg(appId)));
  pump(300);

  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest,
       SocketStartHonorsBlacklistOnResolvedDisplayName) {
  constexpr uint appId = 1422452;
  FakeSteamHome steam(appId, "Display Blocked Game");
  ASSERT_TRUE(steam.isValid());
  ASSERT_TRUE(service->addToBlacklist("Display Blocked Game"));

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send(QString("START:7242:raw-allowed.exe:%1\n").arg(appId)));
  pump(300);

  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, ReconcileHonorsBlacklistOnRawWindowKey) {
  constexpr uint appId = 1422453;
  FakeSteamHome steam(appId, "Display Allowed Game");
  ASSERT_TRUE(steam.isValid());
  ASSERT_TRUE(service->addToWhitelist(QString("steam_app_%1").arg(appId), ""));
  ASSERT_TRUE(service->addToBlacklist(QString("steam_app_%1").arg(appId)));

  WindowInfo w;
  w.id = 7243;
  w.appId = QString("steam_app_%1").arg(appId);
  w.title = "Display Allowed Game";
  w.pid = QCoreApplication::applicationPid();
  w.focused = true;
  w.fullscreen = true;
  monitor.ingestSnapshot({w});
  pump(100);

  EXPECT_TRUE(service->currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest,
       ReconcileHonorsBlacklistOnResolvedDisplayName) {
  constexpr uint appId = 1422454;
  FakeSteamHome steam(appId, "Display Blocked Game");
  ASSERT_TRUE(steam.isValid());
  ASSERT_TRUE(service->addToWhitelist(QString("steam_app_%1").arg(appId), ""));
  ASSERT_TRUE(service->addToBlacklist("Display Blocked Game"));

  WindowInfo w;
  w.id = 7244;
  w.appId = QString("steam_app_%1").arg(appId);
  w.title = "Display Blocked Game";
  w.pid = QCoreApplication::applicationPid();
  w.focused = true;
  w.fullscreen = true;
  monitor.ingestSnapshot({w});
  pump(100);

  EXPECT_TRUE(service->currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, ProcessExistsReflectsLiveness) {
  EXPECT_TRUE(
      GameDetectionService::processExists(QCoreApplication::applicationPid()));
  EXPECT_FALSE(GameDetectionService::processExists(0));
  EXPECT_FALSE(GameDetectionService::processExists(-7));

  // A spawned child exists until reaped, then it is gone.
  QProcess child;
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  EXPECT_TRUE(GameDetectionService::processExists(pid));
  child.kill();
  ASSERT_TRUE(child.waitForFinished(5000));
  EXPECT_FALSE(GameDetectionService::processExists(pid));
}

TEST_F(GameDetectionServiceTest, LivenessWatchdogClosesDeadGame) {
  // Live regression (2026-10-02): quitting Deadlock produced no STOP
  // (no vkDestroyInstance on quit) and the socket lingered, so the
  // session outlived the game by 8s of dead air. The held socket here
  // silences the disconnect path on purpose: only the watchdog can
  // close the game.
  service->addToWhitelist("Liveness Game", "Liveness Game");

  QProcess child;
  child.start("sleep", {"60"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();

  GameClient game;
  ASSERT_TRUE(game.connect());
  QSignalSpy detectedSpy(service.get(), &GameDetectionService::gameDetected);
  QSignalSpy closedSpy(service.get(), &GameDetectionService::gameClosed);
  ASSERT_TRUE(game.send(QString("START:%1:Liveness Game\n").arg(pid)));
  pump(300);
  ASSERT_EQ(detectedSpy.count(), 1);
  ASSERT_EQ(service->currentGamePid(), pid);

  child.kill();
  ASSERT_TRUE(child.waitForFinished(5000));

  // Watchdog interval is 5s; the held socket keeps disconnect quiet.
  ASSERT_TRUE(closedSpy.wait(12000));
  EXPECT_EQ(closedSpy.count(), 1);
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(service->currentGamePid(), 0);
}

TEST_F(GameDetectionServiceTest, UnifiedModelListsAndSections) {
  QAbstractItemModel *model = service->gamesModel();
  ASSERT_NE(model, nullptr);

  auto rowByKey = [&](const QString &key) {
    for (int r = 0; r < model->rowCount(); ++r) {
      const QModelIndex i = model->index(r, 0);
      if (model->data(i, GameListModel::KeyRole).toString() == key)
        return i;
    }
    return QModelIndex();
  };

  // Built-in exclusions hide until opted in.
  EXPECT_FALSE(rowByKey("discord").isValid());
  EXPECT_FALSE(service->showDefaultExclusions());
  service->setShowDefaultExclusions(true);
  EXPECT_TRUE(service->showDefaultExclusions());
  QModelIndex sys = rowByKey("discord");
  ASSERT_TRUE(sys.isValid());
  EXPECT_TRUE(model->data(sys, GameListModel::DefaultRole).toBool());
  EXPECT_EQ(model->data(sys, GameListModel::SectionRole).toString(),
            QString("Games"));
  EXPECT_EQ(model->data(sys, GameListModel::ListedRole).toInt(), 2);
  service->setShowDefaultExclusions(false);
  EXPECT_FALSE(rowByKey("discord").isValid());

  // Whitelist add lands in Allowed with roles; duplicate renames in place.
  QSignalSpy spy(service.get(), &GameDetectionService::whitelistChanged);
  EXPECT_TRUE(service->addToWhitelist("model-game.exe", "Model Game"));
  EXPECT_EQ(spy.count(), 1);
  QModelIndex idx = rowByKey("model-game");
  ASSERT_TRUE(idx.isValid());
  EXPECT_EQ(model->data(idx, GameListModel::DisplayRole).toString(),
            QString("Model Game"));
  EXPECT_EQ(model->data(idx, GameListModel::SectionRole).toString(),
            QString("Games"));
  EXPECT_EQ(model->data(idx, GameListModel::ListedRole).toInt(), 1);
  EXPECT_FALSE(model->data(idx, GameListModel::DefaultRole).toBool());

  EXPECT_TRUE(service->addToWhitelist("model-game.exe", "Renamed Game"));
  idx = rowByKey("model-game");
  ASSERT_TRUE(idx.isValid());
  EXPECT_EQ(model->data(idx, GameListModel::DisplayRole).toString(),
            QString("Renamed Game"));
  EXPECT_FALSE(service->addToWhitelist("model-game.exe", "Renamed Game"));
  EXPECT_FALSE(service->addToWhitelist("  ", "blank"));

  // Blacklist add lands in Blocked as a user entry.
  EXPECT_TRUE(service->addToBlacklist("model-block.exe"));
  idx = rowByKey("model-block");
  ASSERT_TRUE(idx.isValid());
  EXPECT_EQ(model->data(idx, GameListModel::SectionRole).toString(),
            QString("Games"));
  EXPECT_EQ(model->data(idx, GameListModel::ListedRole).toInt(), 2);
  EXPECT_FALSE(model->data(idx, GameListModel::DefaultRole).toBool());
  EXPECT_FALSE(service->addToBlacklist("model-block.exe"));

  EXPECT_TRUE(service->removeFromWhitelist("model-game.exe"));
  EXPECT_FALSE(rowByKey("model-game").isValid());
  EXPECT_FALSE(service->removeFromWhitelist("model-game.exe"));
  EXPECT_TRUE(service->removeFromBlacklist("model-block.exe"));
  EXPECT_FALSE(rowByKey("model-block").isValid());
  EXPECT_FALSE(service->removeFromBlacklist("model-block.exe"));
}

TEST_F(GameDetectionServiceTest, MoveBetweenListsIsAtomic) {
  // The Games page moves rows with one call: remove+add must persist
  // together even though each half alone would reset the model. Poles
  // govern the block veto only; listing membership is orthogonal.
  EXPECT_TRUE(service->moveToWhitelist("moveme", "Move Me"));
  EXPECT_TRUE(service->whitelist().contains("moveme"));
  EXPECT_FALSE(service->blacklist().contains("moveme"));
  EXPECT_FALSE(service->moveToWhitelist("moveme", "Move Me"));

  EXPECT_TRUE(service->moveToBlacklist("moveme"));
  EXPECT_TRUE(service->blacklist().contains("moveme"));
  EXPECT_TRUE(service->whitelist().contains("moveme")); // veto wins on read
  EXPECT_FALSE(service->moveToBlacklist("moveme"));
  EXPECT_TRUE(service->unblockGame("moveme"));
  EXPECT_TRUE(service->whitelist().contains("moveme"));
  EXPECT_FALSE(service->moveToWhitelist("  ", "blank"));
  EXPECT_FALSE(service->moveToBlacklist(""));

  QAbstractItemModel *model = service->gamesModel();
  bool allowedRow = false;
  EXPECT_FALSE(service->moveToWhitelist("moveme", "Move Me")); // still listed
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "moveme" &&
        model->data(i, GameListModel::SectionRole).toString() == "Games")
      allowedRow = true;
  }
  EXPECT_TRUE(allowedRow);
}

TEST_F(GameDetectionServiceTest, CustomEntriesStickWithoutRules) {
  // Add-dialog entries stay visible with no rule attached, so Prompt
  // never makes a watched game vanish. Reset first: the fixture loads
  // the real user config, which may already name this game.
  service->forgetGame("factorio");
  EXPECT_TRUE(service->addCustomGame("factorio", "Factorio", false));
  EXPECT_TRUE(service->moveToBlacklist("factorio"));
  {
    // A watched custom with a block veto reads Blocked (veto wins),
    // otherwise the switch looks stuck on Prompt.
    QAbstractItemModel *model = service->gamesModel();
    bool blockedRow = false;
    for (int r = 0; r < model->rowCount(); ++r) {
      const QModelIndex i = model->index(r, 0);
      if (model->data(i, GameListModel::KeyRole).toString() == "factorio" &&
          model->data(i, GameListModel::ListedRole).toInt() == 2)
        blockedRow = true;
    }
    EXPECT_TRUE(blockedRow);
  }
  EXPECT_TRUE(service->unblockGame("factorio"));
  QAbstractItemModel *model = service->gamesModel();
  bool stickyRow = false;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "factorio" &&
        model->data(i, GameListModel::ListedRole).toInt() == 1)
      stickyRow = true;
  }
  EXPECT_TRUE(stickyRow);
  EXPECT_TRUE(service->forgetGame("factorio"));
  bool goneRow = false;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "factorio")
      goneRow = true;
  }
  EXPECT_FALSE(goneRow);
  EXPECT_FALSE(service->addCustomGame("  ", "blank", false));
}

TEST_F(GameDetectionServiceTest, SteamGameKeyForm) {
  EXPECT_FALSE(service->addSteamGame(0, "Zero"));
  EXPECT_TRUE(service->addSteamGame(1422450, "Deadlock"));
  EXPECT_TRUE(service->whitelist().contains("steam_app_1422450"));

  QAbstractItemModel *model = service->gamesModel();
  QModelIndex row;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() ==
        "steam_app_1422450") {
      row = i;
      break;
    }
  }
  ASSERT_TRUE(row.isValid());
  EXPECT_EQ(model->data(row, GameListModel::SteamAppIdRole).toUInt(), 1422450u);
  EXPECT_EQ(model->data(row, GameListModel::SectionRole).toString(),
            QString("Games"));
  EXPECT_TRUE(service->removeFromWhitelist("steam_app_1422450"));
}

TEST_F(GameDetectionServiceTest, UnifiedModelShowsLiveGame) {
  QAbstractItemModel *model = service->gamesModel();
  ASSERT_NE(model, nullptr);
  EXPECT_TRUE(service->addToWhitelist("live-game.exe", "Live Game"));

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:424242:live-game.exe\n"));
  pump();
  ASSERT_EQ(service->currentGameName(), QString("Live Game"));
  EXPECT_EQ(service->currentGameKey(), QString("live-game"));

  // Live row pins first in Now playing with its raw key.
  ASSERT_GE(model->rowCount(), 1);
  const QModelIndex idx = model->index(0, 0);
  EXPECT_EQ(model->data(idx, GameListModel::DisplayRole).toString(),
            QString("Live Game"));
  EXPECT_EQ(model->data(idx, GameListModel::KeyRole).toString(),
            QString("live-game"));
  EXPECT_TRUE(model->data(idx, GameListModel::RunningRole).toBool());
  EXPECT_EQ(model->data(idx, GameListModel::SectionRole).toString(),
            QString("Now playing"));
  EXPECT_EQ(model->data(idx, GameListModel::ListedRole).toInt(), 1);

  // Closing the socket retires the live row; history keeps a stopped row.
  game.socket.disconnectFromServer();
  pump();
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_TRUE(service->currentGameKey().isEmpty());
  // Listed history collapses into its list row (deduped by key): the game
  // survives under Allowed, stopped, with its raw key intact.
  bool allowedRow = false;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "live-game" &&
        !model->data(i, GameListModel::RunningRole).toBool() &&
        model->data(i, GameListModel::SectionRole).toString() == "Games")
      allowedRow = true;
  }
  EXPECT_TRUE(allowedRow);
}

TEST_F(GameDetectionServiceTest, UnblockDropsVetoKeepsListing) {
  // Prompt pole drops the block veto; whitelist membership is orthogonal
  // (the x button governs existence).
  EXPECT_TRUE(service->addToWhitelist("neutral-game", "Neutral Game"));
  EXPECT_FALSE(service->unblockGame("neutral-game"));
  EXPECT_TRUE(service->moveToBlacklist("neutral-game"));
  EXPECT_TRUE(service->unblockGame("neutral-game"));
  EXPECT_TRUE(service->whitelist().contains("neutral-game"));
  EXPECT_FALSE(service->unblockGame(""));

  QAbstractItemModel *model = service->gamesModel();
  auto rowByKey = [&](const QString &key) {
    for (int r = 0; r < model->rowCount(); ++r) {
      const QModelIndex i = model->index(r, 0);
      if (model->data(i, GameListModel::KeyRole).toString() == key)
        return i;
    }
    return QModelIndex();
  };
  // forgetGame is the full delete: lists, custom mark and history.
  EXPECT_TRUE(service->forgetGame("neutral-game"));
  EXPECT_FALSE(rowByKey("neutral-game").isValid());
  EXPECT_FALSE(service->forgetGame("neutral-game"));
  EXPECT_FALSE(service->forgetGame(""));

  // ...but a neutralized built-in stays put showing neutral. Force a
  // known starting state first: the fixture service loads the real user
  // config in its ctor, so a default may already be neutralized live.
  service->unblockGame("discord"); // end neutral whatever the start
  EXPECT_TRUE(service->moveToBlacklist("discord"));
  EXPECT_TRUE(service->unblockGame("discord"));
  service->setShowDefaultExclusions(true); // neutral built-ins hide by default
  QModelIndex idx = rowByKey("discord");
  ASSERT_TRUE(idx.isValid());
  EXPECT_EQ(model->data(idx, GameListModel::ListedRole).toInt(), 0);
  EXPECT_TRUE(model->data(idx, GameListModel::DefaultRole).toBool());
  // ...and hides with the defaults.
  service->setShowDefaultExclusions(false);
  EXPECT_FALSE(rowByKey("discord").isValid());
  service->setShowDefaultExclusions(true);
}

TEST_F(GameDetectionServiceTest, BlockedQueryMirrorsVeto) {
  // The authorize prompt gate: case/.exe variants match, whitelist-only
  // is not blocked, blank never is.
  service->forgetGame("query-game");
  EXPECT_FALSE(service->isBlocked("query-game"));
  EXPECT_TRUE(service->moveToBlacklist("Query-Game.EXE"));
  EXPECT_TRUE(service->isBlocked("query-game"));
  EXPECT_TRUE(service->isBlocked("Query-Game.EXE"));
  EXPECT_TRUE(service->unblockGame("query-game"));
  EXPECT_FALSE(service->isBlocked("query-game"));
  EXPECT_FALSE(service->isBlocked(""));
}

TEST_F(GameDetectionServiceTest, BlockingLiveGameRetiresTracking) {
  // Vetoing the tracked game clears the dashboard (currentGameName) and
  // the row survives as Blocked in the Games pool.
  EXPECT_TRUE(service->addToWhitelist("retire-game.exe", "Retire Game"));
  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:434343:retire-game.exe\n"));
  pump();
  ASSERT_EQ(service->currentGameName(), QString("Retire Game"));

  EXPECT_TRUE(service->moveToBlacklist("retire-game"));
  EXPECT_TRUE(service->currentGameName().isEmpty());

  QAbstractItemModel *model = service->gamesModel();
  bool blockedRow = false;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "retire-game" &&
        model->data(i, GameListModel::ListedRole).toInt() == 2)
      blockedRow = true;
  }
  EXPECT_TRUE(blockedRow);
  game.socket.disconnectFromServer();
  pump();
  EXPECT_TRUE(service->forgetGame("retire-game"));
}

TEST_F(GameDetectionServiceTest, GamesPoolSortsAlphabetically) {
  service->setShowDefaultExclusions(true);
  EXPECT_TRUE(service->addToWhitelist("zebra-game", "Zebra"));
  EXPECT_TRUE(service->addToWhitelist("alpha-game", "Alpha"));
  EXPECT_TRUE(service->addToBlacklist("user-block-z"));
  EXPECT_TRUE(service->addToBlacklist("user-block-a"));

  QAbstractItemModel *model = service->gamesModel();
  QString last;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    ASSERT_EQ(model->data(i, GameListModel::SectionRole).toString(),
              QString("Games"));
    const QString display =
        model->data(i, GameListModel::DisplayRole).toString();
    EXPECT_TRUE(last.isEmpty() ||
                last.compare(display, Qt::CaseInsensitive) <= 0)
        << qPrintable(last + " should precede " + display);
    last = display;
  }
  EXPECT_FALSE(last.isEmpty());
}

TEST_F(GameDetectionServiceTest, TokenGamesFormApprovedSection) {
  EXPECT_TRUE(service->addToWhitelist("token-game", "Token Game"));
  service->setTokenGames({"Token Game"});

  QAbstractItemModel *model = service->gamesModel();
  QModelIndex row;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::KeyRole).toString() == "token-game")
      row = i;
  }
  ASSERT_TRUE(row.isValid());
  EXPECT_EQ(model->data(row, GameListModel::SectionRole).toString(),
            QString("Ready to capture"));
  EXPECT_TRUE(model->data(row, GameListModel::HasTokenRole).toBool());
  EXPECT_EQ(model->data(row, GameListModel::TokenKeyRole).toString(),
            QString("Token Game"));

  // Withdrawing the token drops the row back to the pool.
  service->setTokenGames({});
  bool approvedLeft = false, poolRow = false;
  for (int r = 0; r < model->rowCount(); ++r) {
    const QModelIndex i = model->index(r, 0);
    if (model->data(i, GameListModel::SectionRole).toString() ==
        "Ready to capture")
      approvedLeft = true;
    if (model->data(i, GameListModel::KeyRole).toString() == "token-game" &&
        model->data(i, GameListModel::SectionRole).toString() == "Games")
      poolRow = true;
  }
  EXPECT_FALSE(approvedLeft);
  EXPECT_TRUE(poolRow);
}

TEST_F(GameDetectionServiceTest, LiveTokenedGameShowsReady) {
  EXPECT_TRUE(service->addToWhitelist("live-token-game", "Live Token"));
  service->setTokenGames({"Live Token"});

  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:434343:live-token-game\n"));
  pump();
  ASSERT_EQ(service->currentGameName(), QString("Live Token"));

  QAbstractItemModel *model = service->gamesModel();
  ASSERT_GE(model->rowCount(), 1);
  const QModelIndex idx = model->index(0, 0);
  EXPECT_EQ(model->data(idx, GameListModel::SectionRole).toString(),
            QString("Now playing"));
  EXPECT_TRUE(model->data(idx, GameListModel::RunningRole).toBool());
  EXPECT_TRUE(model->data(idx, GameListModel::HasTokenRole).toBool());
  game.socket.disconnectFromServer();
  pump();
}

TEST_F(GameDetectionServiceTest, GameFilterTextFiltersUnifiedList) {
  QAbstractItemModel *model = service->gamesModel();
  EXPECT_TRUE(service->addToWhitelist("filter-me.exe", "Filter Me"));
  EXPECT_TRUE(service->addToBlacklist("other-block.exe"));
  const int unfiltered = model->rowCount();
  ASSERT_GT(unfiltered, 1);

  service->setGameFilterText("filter-me");
  EXPECT_EQ(model->rowCount(), 1);

  service->setGameFilterText("");
  EXPECT_EQ(model->rowCount(), unfiltered);
}

TEST_F(GameDetectionServiceTest, PageCallsResolveAgainstServiceApi) {
  // The Games page calls gameDetector.<method>(...) from QML, where a
  // renamed C++ method fails silently at click time (no compile error).
  // Parse the page for those call sites and require each name to exist
  // on the service metaobject (Q_INVOKABLE/slot/signal).
  QFile page(QStringLiteral(TROPHY_SOURCE_DIR "/qml/pages/GameListPage.qml"));
  ASSERT_TRUE(page.open(QIODevice::ReadOnly)) << "page missing";
  const QString qml = QString::fromUtf8(page.readAll());
  static const QRegularExpression callRe(
      QStringLiteral("gameDetector\\.([A-Za-z_][A-Za-z0-9_]*)\\s*\\("));
  QSet<QString> names;
  auto it = callRe.globalMatch(qml);
  while (it.hasNext())
    names.insert(it.next().captured(1));
  EXPECT_FALSE(names.isEmpty());
  const QMetaObject *meta = service->metaObject();
  for (const QString &name : names) {
    bool found = false;
    for (int i = 0; i < meta->methodCount(); ++i) {
      if (QString::fromLatin1(meta->method(i).name()) == name) {
        found = true;
        break;
      }
    }
    EXPECT_TRUE(found) << qPrintable(
        QString("GameListPage calls gameDetector.%1() which no longer exists "
                "on GameDetectionService")
            .arg(name));
  }
}

} // namespace TrophyBackend
