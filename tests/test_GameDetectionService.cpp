#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <array>
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

  QString appsDir() const {
    return m_home.path() + "/.local/share/Steam/steamapps";
  }

  bool writeManifest(uint appId, const QString &name, const QDateTime &mtime) {
    QFile manifest(appsDir() + QString("/appmanifest_%1.acf").arg(appId));
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    const QByteArray content =
        QString("\"AppState\" { \"appid\" \"%1\" \"name\" \"%2\" }\n")
            .arg(appId)
            .arg(name)
            .toUtf8();
    return manifest.write(content) == content.size() && manifest.flush() &&
           manifest.setFileTime(mtime, QFileDevice::FileModificationTime);
  }

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
    // The reporter path is frozen, so TMPDIR/XDG isolation cannot move it.
    // Never let tests send fake START/STOP events into a running Trophy.
    QLocalSocket existing;
    existing.connectToServer("/tmp/trophy-detection.sock");
    if (existing.waitForConnected(100)) {
      existing.disconnectFromServer();
      GTEST_SKIP() << "A live listener owns the frozen detection socket";
    }
    // GameDetectionService persists whitelist/blacklist to
    // XDG_CONFIG_HOME/trophy — keep tests off the user's real config.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    service = std::make_unique<GameDetectionService>();
    service->setWindowMonitor(&monitor);
    service->start();
  }

  void TearDown() override {
    if (!service)
      return;
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

  static bool waitForSteamAppId(qint64 pid, uint appId) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (GameDetectionService::steamAppIdForPid(pid) == appId)
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

  // Private access stays in the already-friended fixture, not TEST_F's
  // generated subclasses. Seed state, then exercise production handlers.
  void seedTrackedGame(qint64 pid, const QString &name, const QString &key,
                       bool current = false) {
    service->m_pidToGameName.insert(pid, name);
    service->m_pidToRawKey.insert(pid, key);
    if (current)
      service->updateCurrentGame(name, pid, key);
  }

  void seedPendingGame(qint64 pid) {
    service->m_pendingCandidates.insert(pid, "Pending");
    service->m_pendingRawKeys.insert(pid, "pending.exe");
  }

  bool hasPidState(qint64 pid) const {
    return service->m_pidToGameName.contains(pid) ||
           service->m_pidToRawKey.contains(pid) ||
           service->m_pendingCandidates.contains(pid) ||
           service->m_pendingRawKeys.contains(pid);
  }

  void livenessTimeout() { service->onLivenessTimeout(); }
  qint64 failoverTarget() const { return service->lowestLiveTrackedPid(); }
  void selectFocused() { service->onMonitorWindowsChanged(); }
  void track(qint64 pid, const QString &name, const QString &key) {
    service->trackGame(pid, name, key);
  }
  void checkIdentity(const QString &reporter, const QString &exe,
                     const QStringList &argv, uint steamId,
                     const QString &key, const QString &name, bool ignored = false,
                     const QString &prefix = {}) {
    const auto identity = GameDetectionService::resolveReporterIdentity(
        reporter, exe, argv, steamId, prefix);
    EXPECT_EQ(identity.ignored, ignored);
    if (!ignored) {
      EXPECT_EQ(identity.key, key);
      EXPECT_EQ(identity.name, name);
    }
  }
  bool livenessTimerActive() const {
    return service->m_livenessTimer->isActive();
  }
  QString steamName(uint appId) { return service->getSteamAppName(appId); }
  bool looksLikeSteam(qint64 pid) {
    return service->processLooksLikeSteamGame(pid);
  }
  void ageSteamVerdict(qint64 pid, bool verdict, bool expired) {
    service->m_steamCheckCache.insert(
        pid, qMakePair(verdict, service->m_uptimeClock.elapsed() -
                                  (expired ? service->kCacheTtlMs : 0)));
  }

  class JoinedChild : public QProcess {
  public:
    ~JoinedChild() override {
      if (state() != QProcess::NotRunning) {
        kill();
        waitForFinished(-1);
      }
    }
  };
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

TEST_F(GameDetectionServiceTest, DefaultReverseDnsAliasVetoesSocketStartUntilUnblocked) {
  FakeSteamHome steam(424242, "com.obsproject.Studio");
  ASSERT_TRUE(steam.isValid());
  QSignalSpy detected(service.get(), &GameDetectionService::gameDetected);
  // A wire app ID supplies confidence, so only the default veto prevents
  // detection. The frozen protocol and targeted-PID resolution are unchanged.
  GameClient app;
  ASSERT_TRUE(app.connect());
  ASSERT_TRUE(app.send("START:7191:com.obsproject.Studio:424242\n"));
  pump(100);
  EXPECT_EQ(detected.count(), 0);
  EXPECT_FALSE(hasPidState(7191));
  EXPECT_TRUE(service->isBlocked("com.obsproject.Studio"));

  ASSERT_TRUE(service->unblockGame("obs"));
  EXPECT_FALSE(service->isBlocked("com.obsproject.Studio"));
  ASSERT_TRUE(app.send("START:7191:com.obsproject.Studio:424242\n"));
  pump(100);
  ASSERT_EQ(detected.count(), 1);
  EXPECT_EQ(service->currentGamePid(), 7191);

  QSignalSpy closed(service.get(), &GameDetectionService::gameClosed);
  ASSERT_TRUE(service->moveToBlacklist("obs"));
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_EQ(closed.count(), 1);
}

TEST_F(GameDetectionServiceTest, ReblockingDefaultRetiresItsNativeExecutableIdentity) {
  ASSERT_TRUE(service->unblockGame("obs"));
  seedTrackedGame(7192, "OBS Studio", "exe:/usr/bin/obs", true);
  QSignalSpy closed(service.get(), &GameDetectionService::gameClosed);
  ASSERT_TRUE(service->moveToBlacklist("com.obsproject.Studio"));
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_FALSE(hasPidState(7192));
  EXPECT_EQ(closed.count(), 1);
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
  // A pid above Linux's maximum pid_max can never name a live process, so
  // the reporter key cannot resolve to whatever happens to own 5555 today
  // (a running Discord once did, which broke this test).
  ASSERT_TRUE(game.send("START:9999999:Real Game\n"));
  pump(300);
  ASSERT_EQ(service->currentGameName(), "Real Game");

  // The Vulkan layer's generic fallback is blacklisted, so it must neither
  // clobber the specific name nor create a second detection.
  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);
  ASSERT_TRUE(sendLine("START:9999999:Unknown Vulkan Game:0\n"));
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

TEST_F(GameDetectionServiceTest, SteamAppIdReportKeysOffSteamAppId) {
  // Regression (phantom Living Hell prompt): null-name Vulkan reporters
  // (the Proton/DXVK norm) must not share one match key. With a Steam id
  // present the stored key is steam_app_<id>, so two games never alias to
  // whichever authorized last.
  constexpr uint firstId = 610101;
  constexpr uint secondId = 610102;
  FakeSteamHome steam(firstId, "First Placeholder Game");
  ASSERT_TRUE(steam.isValid());
  {
    QFile second(steam.appsDir() +
                 QString("/appmanifest_%1.acf").arg(secondId));
    ASSERT_TRUE(second.open(QIODevice::WriteOnly | QIODevice::Text));
    second.write(QString("\"AppState\" { \"appid\" \"%1\" \"name\" "
                        "\"Second Placeholder Game\" }\n")
                     .arg(secondId)
                     .toUtf8());
  }
  // Identity resolution must precede the shared placeholder's default
  // veto: users should not have to unblock it to detect real Steam games.
  EXPECT_TRUE(service->isBlocked("unknown vulkan game"));

  QSignalSpy spy(service.get(), &GameDetectionService::gameDetected);
  GameClient first;
  ASSERT_TRUE(first.connect());
  ASSERT_TRUE(first.send(
      QString("START:6101:Unknown Vulkan Game:%1\n").arg(firstId)));
  pump(300);
  ASSERT_EQ(service->currentGameName(), "First Placeholder Game");
  EXPECT_EQ(service->currentGameKey(),
            QString("steam_app_%1").arg(firstId));

  GameClient second;
  ASSERT_TRUE(second.connect());
  ASSERT_TRUE(second.send(
      QString("START:6102:Unknown Vulkan Game:%1\n").arg(secondId)));
  pump(300);
  ASSERT_EQ(service->currentGameName(), "Second Placeholder Game");
  EXPECT_EQ(service->currentGameKey(),
            QString("steam_app_%1").arg(secondId));
  EXPECT_EQ(spy.count(), 2);
}

TEST_F(GameDetectionServiceTest, GenericNativeReportUsesExecutableIdentity) {
  checkIdentity("Unknown Vulkan Game", "/opt/Games/NativeGame", {}, 0,
                "exe:/opt/games/nativegame", "NativeGame");
}

TEST_F(GameDetectionServiceTest, WineIdentityUsesGameNotLoaderOrArguments) {
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64-preloader",
                {"C:\\GOG Games\\My Game\\Game.exe", "--fullscreen"}, 0,
                "wine::c:/gog games/my game/game", "Game.exe");
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64",
                {"wine64", "/games/Other Game.exe"}, 0,
                "wine::/games/other game", "Other Game.exe");
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64",
                {"wine64", "--debug", "/games/NotTheProcess.exe"}, 0, "", "", true);
}

TEST_F(GameDetectionServiceTest, SharedEngineNameUsesDistinctExecutables) {
  checkIdentity("Unity", "/games/First.exe", {}, 0, "exe:/games/first", "First.exe");
  checkIdentity("Unity", "/games/Second.exe", {}, 0, "exe:/games/second", "Second.exe");
}

TEST_F(GameDetectionServiceTest, SameExecutableBasenameDoesNotAliasAcrossInstallations) {
  checkIdentity("Unknown Vulkan Game", "/games/First/Game.exe", {}, 0,
                "exe:/games/first/game", "Game.exe");
  checkIdentity("Unknown Vulkan Game", "/games/Second/Game.exe", {}, 0,
                "exe:/games/second/game", "Game.exe");
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64", {"C:\\Game.exe"}, 0,
                "wine:/prefix/first:c:/game", "Game.exe", false, "/prefix/first");
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64", {"C:\\Game.exe"}, 0,
                "wine:/prefix/second:c:/game", "Game.exe", false, "/prefix/second");
}

TEST_F(GameDetectionServiceTest, SameDisplayNameDifferentIdentitiesStillSwitchLifecycle) {
  QSignalSpy detected(service.get(), &GameDetectionService::gameDetected);
  QSignalSpy closed(service.get(), &GameDetectionService::gameClosed);
  track(6303, "Game.exe", "exe:/games/first/game");
  track(6304, "Game.exe", "exe:/games/second/game");
  EXPECT_EQ(service->currentGameKey(), "exe:/games/second/game");
  EXPECT_EQ(detected.count(), 2);
  EXPECT_EQ(closed.count(), 1);
}

TEST_F(GameDetectionServiceTest, HelperSteamEnvironmentDoesNotMakeGame) {
  checkIdentity("Unknown Vulkan Game", "/usr/bin/wine64",
                {"C:\\windows\\system32\\d3ddriverquery64.exe"}, 1422450,
                "", "", true);
  checkIdentity("wineopenxr test instance", "/games/game.exe", {}, 1422450,
                "", "", true);
  checkIdentity("Unknown Vulkan Game", "/usr/bin/steamwebhelper", {}, 1422450,
                "", "", true);
}

TEST_F(GameDetectionServiceTest, PrerequisiteInstallersIgnoredBeforeSteamIdentity) {
  const QStringList installers{
      "VC_REDIST.X86.EXE", "vc_redist.x64.exe", "vc_redist.arm64.exe",
      "vcredist_x86.exe", "vcredist_x64.exe", "vcredist_arm64.exe",
      "vcredist_ia64.exe", "DXSETUP.EXE"};
  for (const QString &installer : installers) {
    SCOPED_TRACE(installer.toStdString());
    for (uint steamId : {0u, 493520u}) {
      checkIdentity("steam_proton", "/usr/bin/wine64",
                    {"wine64", "Z:\\Steamworks Shared\\_CommonRedist\\" + installer},
                    steamId, "", "", true, "/compatdata/493520/pfx");
      checkIdentity("steam_proton", "/games/" + installer, {}, steamId,
                    "", "", true);
      checkIdentity(installer, "/games/game.exe", {}, steamId, "", "", true);
    }
  }
}

TEST_F(GameDetectionServiceTest, PrerequisiteFilterDoesNotExcludeGamesOrGenericSetup) {
  for (const QString &executable : {QString("Game.exe"), QString("setup.exe"),
                                    QString("vc_redist.x64-adventure.exe")}) {
    checkIdentity("steam_proton", "/usr/bin/wine64",
                  {"wine64", "C:\\Games\\" + executable}, 493520,
                  "steam_app_493520", "steam_proton");
  }
}

TEST_F(GameDetectionServiceTest, BackgroundReporterCannotStealVisibleGame) {
  auto currentWindow = snapshotFor(6301, true, true);
  monitor.ingestSnapshot(currentWindow);
  track(6301, "Playing", "playing");
  track(6302, "Background", "background");
  EXPECT_EQ(service->currentGameName(), "Playing");
  auto otherWindow = snapshotFor(6302, true, true);
  currentWindow[0].focused = false;
  monitor.ingestSnapshot({currentWindow.first(), otherWindow.first()});
  EXPECT_EQ(service->currentGameName(), "Background");
}

TEST_F(GameDetectionServiceTest, BlockingIdentityRetiresAllItsProcesses) {
  seedTrackedGame(6401, "Same Game", "same-game", true);
  seedTrackedGame(6402, "Same Game", "same-game");
  ASSERT_TRUE(service->addToBlacklist("same-game"));
  EXPECT_FALSE(hasPidState(6401));
  EXPECT_FALSE(hasPidState(6402));
  monitor.ingestSnapshot(snapshotFor(6402, true, true));
  EXPECT_TRUE(service->currentGameName().isEmpty());
}

TEST_F(GameDetectionServiceTest, GenericNativeSocketReportResolvesBeforeDefaultVeto) {
  const qint64 pid = QCoreApplication::applicationPid();
  ASSERT_EQ(GameDetectionService::steamAppIdForPid(pid), 0u);
  // No Steam or whitelist evidence: native executable identity is not
  // proof of a game, so it still needs the focused/fullscreen gate.
  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send(QString("START:%1:Unknown Vulkan Game:0\n").arg(pid)));
  pump(100);
  EXPECT_TRUE(service->currentGameName().isEmpty());
  monitor.ingestSnapshot(snapshotFor(pid, true, true));
  pump(100);
  EXPECT_EQ(service->currentGamePid(), pid);
  EXPECT_EQ(service->currentGameKey(),
            "exe:" + QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget().toLower());
  EXPECT_NE(service->currentGameName(), "Unknown Vulkan Game");
}

TEST_F(GameDetectionServiceTest, SteamHelperRejectedWithoutUnblockingPlaceholder) {
  GameClient helper;
  ASSERT_TRUE(helper.connect());
  ASSERT_TRUE(helper.send("START:6501:d3ddriverquery64.exe:1422450\n"));
  pump(100);
  EXPECT_TRUE(service->currentGameName().isEmpty());
  EXPECT_FALSE(hasPidState(6501));
}

TEST_F(GameDetectionServiceTest, FailoverPrefersSameIdentityThenFocusedGame) {
  JoinedChild other;
  JoinedChild successor;
  other.start("sleep", {"60"});
  successor.start("sleep", {"60"});
  ASSERT_TRUE(other.waitForStarted());
  ASSERT_TRUE(successor.waitForStarted());
  seedTrackedGame(other.processId(), "Other", "other");
  seedTrackedGame(successor.processId(), "Playing", "playing");
  seedTrackedGame(2147483647, "Playing", "playing", true);
  EXPECT_EQ(failoverTarget(), successor.processId());
  monitor.ingestSnapshot(snapshotFor(other.processId(), true, true));
  // Reset only the representative to emulate its death after the
  // snapshot: focused evidence must outrank a background same-game PID.
  seedTrackedGame(2147483647, "Playing", "playing", true);
  EXPECT_EQ(failoverTarget(), other.processId());
}

TEST_F(GameDetectionServiceTest, UnrelatedStopCannotRetireReporter) {
  ASSERT_TRUE(service->addToWhitelist("Held Game", "Held Game"));
  GameClient game;
  ASSERT_TRUE(game.connect());
  ASSERT_TRUE(game.send("START:6502:Held Game:0\n"));
  pump(100);
  GameClient stranger;
  ASSERT_TRUE(stranger.connect());
  ASSERT_TRUE(stranger.send("STOP:6502\n"));
  pump(100);
  EXPECT_EQ(service->currentGamePid(), 6502);
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

TEST_F(GameDetectionServiceTest, LivenessFailoverSkipsDeadTrackedPid) {
  JoinedChild first, second, third;
  std::array<JoinedChild *, 3> children{&first, &second, &third};
  for (auto *child : children) {
    child->start("sleep", {"60"});
    ASSERT_TRUE(child->waitForStarted(5000));
  }
  std::sort(children.begin(), children.end(), [](const auto *a, const auto *b) {
    return a->processId() < b->processId();
  });
  auto &deadCandidate = *children[0];
  auto &survivor = *children[1];
  auto &current = *children[2];
  const qint64 deadPid = deadCandidate.processId();
  const qint64 livePid = survivor.processId();
  const qint64 currentPid = current.processId();
  // Sorting avoids assuming sequential PID allocation: the deliberately
  // dead map entry sorts before the survivor to catch firstKey() failover.

  seedTrackedGame(deadPid, "Dead Candidate", "dead.exe");
  seedTrackedGame(livePid, "Surviving Game", "survivor.exe");
  seedTrackedGame(currentPid, "Closing Game", "closing.exe", true);
  seedPendingGame(currentPid);
  QSignalSpy closed(service.get(), &GameDetectionService::gameClosed);
  QSignalSpy detected(service.get(), &GameDetectionService::gameDetected);
  // Own the log through connections too, including assertion-failure teardown.
  auto events = std::make_shared<QStringList>();
  QObject::connect(
      service.get(), &GameDetectionService::gameClosed, service.get(),
      [events](const QString &name) { *events << "closed:" + name; });
  QObject::connect(service.get(), &GameDetectionService::gameDetected,
                   service.get(), [events](const QString &name, qint64) {
                     *events << "detected:" + name;
                   });

  livenessTimeout(); // The current process is still alive: no transition.
  EXPECT_EQ(closed.count(), 0);
  EXPECT_EQ(detected.count(), 0);
  deadCandidate.kill();
  ASSERT_TRUE(deadCandidate.waitForFinished(5000));
  current.kill();
  ASSERT_TRUE(current.waitForFinished(5000));
  ASSERT_FALSE(GameDetectionService::processExists(deadPid));
  ASSERT_FALSE(GameDetectionService::processExists(currentPid));
  ASSERT_TRUE(GameDetectionService::processExists(livePid));

  livenessTimeout();
  EXPECT_EQ(service->currentGamePid(), livePid);
  EXPECT_EQ(service->currentGameName(), "Surviving Game");
  EXPECT_EQ(service->currentGameKey(), "survivor.exe");
  EXPECT_FALSE(hasPidState(currentPid));
  EXPECT_TRUE(livenessTimerActive());
  ASSERT_EQ(closed.count(), 1);
  EXPECT_EQ(closed.at(0).at(0).toString(), "Closing Game");
  ASSERT_EQ(detected.count(), 1);
  EXPECT_EQ(detected.at(0).at(1).toLongLong(), livePid);
  EXPECT_EQ(*events,
            (QStringList{"closed:Closing Game", "detected:Surviving Game"}));

  livenessTimeout(); // Repeated ticks must not re-announce the survivor.
  EXPECT_EQ(closed.count(), 1);
  EXPECT_EQ(detected.count(), 1);
  survivor.kill();
  ASSERT_TRUE(survivor.waitForFinished(5000));
  livenessTimeout();
  EXPECT_EQ(service->currentGamePid(), 0);
  EXPECT_TRUE(service->currentGameKey().isEmpty());
  EXPECT_FALSE(hasPidState(deadPid));
  EXPECT_FALSE(hasPidState(livePid));
  EXPECT_FALSE(livenessTimerActive());
  EXPECT_EQ(closed.count(), 2);
  EXPECT_EQ(detected.count(), 1);
}

TEST_F(GameDetectionServiceTest, SteamNameCacheInvalidatesOnManifestMtime) {
  constexpr uint appId = 987650;
  FakeSteamHome steam(appId, "Original Name");
  ASSERT_TRUE(steam.isValid());
  const QDateTime before = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
  ASSERT_TRUE(steam.writeManifest(appId, "Original Name", before));
  const QString libraries = steam.appsDir() + "/libraryfolders.vdf";
  const QDateTime libraryMtime = QFileInfo(libraries).lastModified();
  EXPECT_EQ(steamName(appId), "Original Name");
  EXPECT_EQ(steamName(appId), "Original Name");

  ASSERT_TRUE(steam.writeManifest(appId, "Updated Name", before.addSecs(10)));
  ASSERT_EQ(QFileInfo(libraries).lastModified(), libraryMtime);
  EXPECT_EQ(steamName(appId), "Updated Name");
  ASSERT_TRUE(QFile::remove(steam.appsDir() +
                           QString("/appmanifest_%1.acf").arg(appId)));
  EXPECT_TRUE(steamName(appId).isEmpty());
  // A miss must not be cached: installation does not have to touch the VDF.
  ASSERT_TRUE(steam.writeManifest(appId, "Installed Again", before.addSecs(20)));
  EXPECT_EQ(steamName(appId), "Installed Again");
}

TEST_F(GameDetectionServiceTest, SteamNameCacheInvalidatesOnLibraryMtime) {
  constexpr uint appId = 987651;
  FakeSteamHome steam(appId, "Original Library");
  ASSERT_TRUE(steam.isValid());
  const QDateTime manifestMtime = QDateTime::fromMSecsSinceEpoch(1700000000000LL);
  ASSERT_TRUE(steam.writeManifest(appId, "Original Library", manifestMtime));
  EXPECT_EQ(steamName(appId), "Original Library");
  // Hold the manifest mtime unchanged, isolating the library-key invalidation.
  ASSERT_TRUE(steam.writeManifest(appId, "Updated Library", manifestMtime));
  EXPECT_EQ(steamName(appId), "Original Library");
  QFile libraries(steam.appsDir() + "/libraryfolders.vdf");
  const QDateTime previousMtime = QFileInfo(libraries).lastModified();
  ASSERT_TRUE(libraries.open(QIODevice::ReadWrite));
  ASSERT_TRUE(libraries.setFileTime(previousMtime.addSecs(10),
                                   QFileDevice::FileModificationTime));
  libraries.close();
  EXPECT_EQ(steamName(appId), "Updated Library");
}

TEST_F(GameDetectionServiceTest, SteamVerdictNegativeCacheExpiresAtTtl) {
  JoinedChild child;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("SteamAppId", "987652");
  child.setProcessEnvironment(environment);
  child.start("sleep", {"60"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  ASSERT_TRUE(waitForSteamAppId(pid, 987652u));
  ASSERT_EQ(GameDetectionService::steamAppIdForPid(pid), 987652u);
  // Inject only cache age/verdict, never the lookup result or clock itself.
  ageSteamVerdict(pid, false, false);
  EXPECT_FALSE(looksLikeSteam(pid));
  ageSteamVerdict(pid, false, true);
  EXPECT_TRUE(looksLikeSteam(pid));
  EXPECT_TRUE(looksLikeSteam(pid));
}

TEST_F(GameDetectionServiceTest, SteamVerdictPositiveCacheExpiresAtTtl) {
  JoinedChild child;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("SteamAppId", "987653");
  child.setProcessEnvironment(environment);
  child.start("sleep", {"60"});
  ASSERT_TRUE(child.waitForStarted(5000));
  const qint64 pid = child.processId();
  ASSERT_TRUE(waitForSteamAppId(pid, 987653u));
  ASSERT_EQ(GameDetectionService::steamAppIdForPid(pid), 987653u);
  EXPECT_TRUE(looksLikeSteam(pid));
  child.kill();
  ASSERT_TRUE(child.waitForFinished(5000));
  ASSERT_FALSE(GameDetectionService::processExists(pid));
  ageSteamVerdict(pid, true, false);
  EXPECT_TRUE(looksLikeSteam(pid)); // Fresh positive intentionally stays cached.
  ageSteamVerdict(pid, true, true);
  EXPECT_FALSE(looksLikeSteam(pid));
  EXPECT_FALSE(looksLikeSteam(pid));
}

TEST_F(GameDetectionServiceTest, RecordingIconsUseUnfilteredUnambiguousGameNames) {
  EXPECT_TRUE(service->iconSourceForGame("").isEmpty());
  EXPECT_EQ(service->iconSourceForGame("unlisted"), gameIconSource("unlisted"));
  ASSERT_TRUE(service->addSteamGame(1422450, "Icon Game"));
  service->setGameFilterText("nothing matches this filter");
  EXPECT_EQ(service->iconSourceForGame("icon game"),
            gameIconSource("steam_app_1422450", 1422450));
  ASSERT_TRUE(service->addSteamGame(42, "Icon Game"));
  EXPECT_TRUE(service->iconSourceForGame("Icon Game").isEmpty());
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
            QString("Apps"));
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
            QString("Apps"));
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
            QString("Apps"));
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

TEST_F(GameDetectionServiceTest, DefaultRowsConsolidateAndSearchTheirIdentifiers) {
  service->setShowDefaultExclusions(true);
  auto *model = service->gamesModel();
  const QList<QPair<QString, QString>> groups{
      {"obs", "obs64"}, {"signal", "signal-desktop"},
      {"telegram", "telegram-desktop"}, {"vkcube", "vkcube-wayland"},
      {"vc_redist", "vc_redist.x86"}};
  for (const auto &[canonical, legacy] : groups) {
    SCOPED_TRACE(legacy.toStdString());
    int count = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
      const auto index = model->index(row, 0);
      const auto key = model->data(index, GameListModel::KeyRole).toString();
      EXPECT_NE(key, legacy);
      if (key == canonical) {
        ++count;
        EXPECT_TRUE(model->data(index, GameListModel::DefaultRole).toBool());
        EXPECT_TRUE(model->data(index, GameListModel::IdentifiersRole).toStringList().contains(legacy));
      }
    }
    EXPECT_EQ(count, 1);
    service->setGameFilterText(legacy);
    ASSERT_EQ(model->rowCount(), 1);
    EXPECT_EQ(model->data(model->index(0, 0), GameListModel::KeyRole).toString(), canonical);
    service->setGameFilterText("");
    ASSERT_TRUE(service->unblockGame(legacy));
    EXPECT_FALSE(service->isBlocked(canonical));
    EXPECT_FALSE(service->isBlocked(legacy));
  }
}

TEST_F(GameDetectionServiceTest, MoveBetweenListsIsAtomic) {
  // The Apps page moves rows with one call: remove+add must persist
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
        model->data(i, GameListModel::SectionRole).toString() == "Apps")
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

TEST_F(GameDetectionServiceTest, PromptBlockActionsKeepExistingGameModelRows) {
  ASSERT_TRUE(service->addCustomGame("scroll-fixture", "Scroll Fixture", false));
  auto *model = service->gamesModel();
  QPersistentModelIndex retained;
  for (int row = 0; row < model->rowCount(); ++row) {
    const auto index = model->index(row, 0);
    if (index.data(GameListModel::KeyRole).toString() == "scroll-fixture")
      retained = index;
  }
  ASSERT_TRUE(retained.isValid());
  QSignalSpy resets(model, &QAbstractItemModel::modelReset);
  for (int repeat = 0; repeat < 3; ++repeat) {
    ASSERT_TRUE(service->moveToBlacklist("scroll-fixture"));
    ASSERT_TRUE(retained.isValid());
    EXPECT_EQ(retained.data(GameListModel::ListedRole).toInt(), 2);
    ASSERT_TRUE(service->unblockGame("scroll-fixture"));
    ASSERT_TRUE(retained.isValid());
    EXPECT_NE(retained.data(GameListModel::ListedRole).toInt(), 2);
  }
  EXPECT_EQ(resets.size(), 0);
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
            QString("Apps"));
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
        model->data(i, GameListModel::SectionRole).toString() == "Apps")
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
  // the row survives as Blocked in the Apps pool.
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

TEST_F(GameDetectionServiceTest, AppsPoolSortsAlphabetically) {
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
              QString("Apps"));
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
        model->data(i, GameListModel::SectionRole).toString() == "Apps")
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
