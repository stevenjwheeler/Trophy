/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/GameDetectionService.h"
#include "detection/GameWhitelist.h"
#include "detection/LegacyDetectionCleanup.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>
#include <unistd.h>

namespace TrophyBackend {
class GameDetectionServiceTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "process-detection-test";
      static char *argv[] = {name, nullptr};
      static QCoreApplication app(argc, argv);
    }
    QStandardPaths::setTestModeEnabled(true);
  }
  static ProcessInfo process(qint64 pid, QString exe, qint64 parent = 1,
                             uint steam = 0) {
    ProcessInfo p;
    p.pid = pid;
    p.start = pid * 10;
    p.parent = parent;
    p.executable = exe;
    p.arguments = {exe};
    p.steamId = steam;
    return p;
  }
  static void apply(GameDetectionService &service,
                    const ProcessCensus::Snapshot &ps) {
    service.apply(ps);
  }
  static void define(GameDetectionService &service, const QString &key,
                     const QString &name) {
    service.m_whitelist->addToWhitelist(key, name);
  }
};

TEST_F(GameDetectionServiceTest,
       ProcessPickerListsPayloadsAndRejectsStaleRules) {
  GameDetectionService service;
  EXPECT_TRUE(service.runningProcesses().isEmpty());
  QProcess child;
  child.start("sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  auto p = ProcessCensus::inspect(child.processId());
  ASSERT_GT(p.pid, 0);
  apply(service, {{p.pid, p}});
  auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].toMap()["pid"].toLongLong(), p.pid);
  EXPECT_EQ(rows[0].toMap()["path"].toString(), p.payload());
  EXPECT_EQ(rows[0].toMap()["identity"].toString(), p.identity());
  EXPECT_FALSE(
      service.addProcessGame(p.pid, p.identity() + "0", "Fixture", false));
  ASSERT_TRUE(service.addProcessGame(p.pid, p.identity(), "Fixture", false));
  const auto key = "exe:" + p.payload();
  EXPECT_EQ(service.currentGameName(), "Fixture");
  EXPECT_TRUE(service.removeFromWhitelist(key));
  EXPECT_TRUE(service.addProcessGame(p.pid, p.identity(), "Fixture", true));
  EXPECT_TRUE(service.isBlocked(key));
  EXPECT_TRUE(service.removeFromBlacklist(key));
  auto changed = p;
  changed.executable += "-different";
  apply(service, {{p.pid, changed}});
  EXPECT_FALSE(
      service.addProcessGame(p.pid, p.identity(), "Wrong payload", false));
  apply(service, {{p.pid, p}});
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(service.addProcessGame(p.pid, p.identity(), "Exited", false));
}

TEST_F(GameDetectionServiceTest, ProcessPickerExposesWinePayloadNotLoader) {
  auto p = process(20, "/usr/bin/wine64-preloader");
  p.arguments = {"C:\\Games\\Game.exe"};
  p.winePrefix = "/prefix";
  auto browser = process(21, "/usr/bin/firefox");
  GameDetectionService service;
  apply(service, {{20, p}, {21, browser}});
  const auto rows = service.runningProcesses();
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].toMap()["path"].toString(),
            "/prefix/drive_c/Games/Game.exe");
  EXPECT_FALSE(
      service.addProcessGame(20, p.identity(), "Not a live process", false));
  const QList<SteamGame> steam{{42, "Fixture", "/prefix/drive_c/Games"}};
  const auto blocked =
      GameWhitelist::normalizeKey("wine:/prefix:" + p.payload());
  EXPECT_TRUE(
      ProcessGames::identify({{20, p}}, steam, {}, {blocked}).isEmpty());
  EXPECT_EQ(ProcessGames::identify(
                {{20, p}}, steam, {},
                {GameWhitelist::normalizeKey("wine:/other:" + p.payload())})
                .size(),
            1);
}

TEST_F(GameDetectionServiceTest,
       NativeSteamRequiresPayloadNotInheritedEnvironment) {
  ProcessCensus::Snapshot ps;
  ps[10] = process(10, "/usr/bin/steam");
  ps[20] = process(20, "/usr/bin/python3", 10, 42);
  ps[30] = process(30, "/games/Native/bin/game", 20, 42);
  ps[31] = process(31, "/games/Native/audio-helper", 30, 42);
  ps[40] = process(40, "/usr/bin/firefox", 20, 42);
  const auto games =
      ProcessGames::identify(ps, {{42, "Native", "/games/Native"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].name, "Native");
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{30, 31}));
}
TEST_F(GameDetectionServiceTest, NativeStandaloneExactUserRule) {
  const auto p = process(10, "/opt/standalone/game");
  const auto games =
      ProcessGames::identify({{10, p}}, {}, {{"game", "Standalone"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].name, "Standalone");
  EXPECT_TRUE(
      ProcessGames::identify({{10, p}}, {}, {{"gam", "Wrong"}}, {}).isEmpty());
}

TEST_F(GameDetectionServiceTest, InstalledNativeGameWithoutSteamEnvironment) {
  const auto p = process(10, "/games/Native/game");
  const auto games = ProcessGames::identify(
      {{10, p}}, {{42, "Native", "/games/Native"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].key, "steam_app_42");
}

TEST_F(GameDetectionServiceTest,
       NativeInterpretedGamesUseConcretePayloadNotAllJavaOrPython) {
  auto java = process(10, "/usr/bin/java");
  java.arguments = {"java", "-jar", "/games/minecraft.jar"};
  auto python = process(20, "/usr/bin/python3");
  python.arguments = {"python3", "/games/standalone.py"};
  auto unrelated = process(30, "/usr/bin/python3");
  unrelated.arguments = {"python3", "-c", "print('not a game')"};
  const auto games = ProcessGames::identify(
      {{10, java}, {20, python}, {30, unrelated}}, {},
      {{"minecraft.jar", "Minecraft"}, {"standalone.py", "Standalone"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].processes.size(), 1);
  EXPECT_EQ(games[1].processes.size(), 1);
}

TEST_F(GameDetectionServiceTest, RapidNativeProcessExitDuringInspectionIsSafe) {
  for (int i = 0; i < 25; ++i) {
    QProcess child;
    child.start("/usr/bin/true");
    ASSERT_TRUE(child.waitForStarted());
    const auto pid = child.processId();
    const auto p = ProcessCensus::inspect(pid);
    EXPECT_TRUE(p.pid == 0 || p.pid == pid);
    ASSERT_TRUE(child.waitForFinished());
    EXPECT_EQ(ProcessCensus::inspect(pid).pid, 0);
  }
}
TEST_F(GameDetectionServiceTest, ProtonWinePayloadAndServices) {
  auto game = process(30, "/usr/bin/wine64-preloader", 20, 42);
  game.arguments = {"Z:\\games\\Windows\\Game.exe"};
  game.directory = "/games/Windows";
  auto service = game;
  service.pid = 31;
  service.arguments = {"C:\\windows\\system32\\services.exe"};
  auto audio = game;
  audio.pid = 32;
  audio.parent = 30;
  audio.arguments = {"Z:\\games\\Windows\\Audio.exe"};
  const auto games =
      ProcessGames::identify({{30, game}, {31, service}, {32, audio}},
                             {{42, "Windows", "/games/Windows"}}, {}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{30, 32}));
}
TEST_F(GameDetectionServiceTest, TwoInstancesOfSameGameRemainSeparate) {
  auto a = process(10, "/games/game");
  auto b = process(20, "/games/game");
  const auto games =
      ProcessGames::identify({{10, a}, {20, b}}, {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_NE(games[0].id, games[1].id);
  auto service = process(10, "/usr/bin/wine64-preloader", 1, 42);
  service.arguments = {"C:\\windows\\system32\\services.exe"};
  auto wineOne = process(20, "/usr/bin/wine64-preloader", 10, 42);
  wineOne.arguments = {"/games/Windows/Game.exe"};
  auto wineTwo = process(30, "/usr/bin/wine64-preloader", 10, 42);
  wineTwo.arguments = wineOne.arguments;
  const auto wineGames =
      ProcessGames::identify({{10, service}, {20, wineOne}, {30, wineTwo}},
                             {{42, "Windows", "/games/Windows"}}, {}, {});
  EXPECT_EQ(wineGames.size(), 2); // A shared Wine service must not merge games.
}
TEST_F(GameDetectionServiceTest, MatchingGrandchildrenStayInTheirRootInstance) {
  const auto root = process(10, "/opt/game");
  const auto child = process(20, "/opt/game", 10);
  const auto grandchild = process(30, "/opt/game", 20);
  const auto games = ProcessGames::identify(
      {{10, root}, {20, child}, {30, grandchild}}, {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.size(), 3);
}
TEST_F(GameDetectionServiceTest, LauncherDoesNotAdoptUnrelatedSiblings) {
  auto launcher = process(10, "/opt/launcher");
  auto a = process(20, "/opt/game-one", 10);
  auto b = process(30, "/opt/game-two", 10);
  auto browser = process(40, "/usr/bin/firefox", 20);
  auto discord = process(50, "/usr/bin/Discord", 20);
  auto games = ProcessGames::identify(
      {{10, launcher}, {20, a}, {30, b}, {40, browser}, {50, discord}}, {},
      {{"game-one", "One"}, {"game-two", "Two"}}, {});
  ASSERT_EQ(games.size(), 2);
  EXPECT_EQ(games[0].processes.size(), 1);
  EXPECT_EQ(games[1].processes.size(), 1);
}
TEST_F(GameDetectionServiceTest,
       ReparentingPreservesIdentityButPidReuseAndExecDoNot) {
  auto a = process(10, "/opt/game");
  auto helper = process(20, "/opt/audio", 10);
  auto games = ProcessGames::identify({{10, a}, {20, helper}}, {},
                                      {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  const auto id = games[0].id;
  helper.parent = 1;
  games =
      ProcessGames::identify({{20, helper}}, {}, {{"game", "Game"}}, {}, games);
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].id, id);
  helper.start++;
  EXPECT_TRUE(
      ProcessGames::identify({{20, helper}}, {}, {}, {}, games).isEmpty());
  helper.start--;
  helper.executable = "/usr/bin/firefox";
  EXPECT_TRUE(
      ProcessGames::identify({{20, helper}}, {}, {}, {}, games).isEmpty());
}
TEST_F(GameDetectionServiceTest, BlockRetiresEntireInstance) {
  const auto p = process(10, "/opt/game");
  EXPECT_TRUE(
      ProcessGames::identify({{10, p}}, {}, {{"game", "Game"}}, {"game"})
          .isEmpty());
}
TEST_F(GameDetectionServiceTest, StatParserHandlesSpacesParenthesesAndZombie) {
  ProcessInfo p;
  QByteArray stat = "123 (game ) with (parens)) S 42";
  for (int field = 5; field <= 21; ++field)
    stat += " 0";
  stat += " 123456 0";
  ASSERT_TRUE(ProcessCensus::parseStat(stat, p));
  EXPECT_EQ(p.parent, 42);
  EXPECT_EQ(p.start, 123456);
  EXPECT_FALSE(ProcessCensus::parseStat("123 (gone) Z 1", p));
  EXPECT_FALSE(ProcessCensus::parseStat("truncated", p));
}
TEST_F(GameDetectionServiceTest, RealProcEnumerationAndExitRace) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto pid = child.processId();
  auto ps = ProcessCensus::scan();
  ASSERT_TRUE(ps.contains(pid));
  EXPECT_GT(ps[pid].start, 0);
  EXPECT_EQ(ps[pid].parent, ::getpid());
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(ProcessCensus::scan().contains(pid));
  EXPECT_EQ(ProcessCensus::inspect(pid).pid, 0);
}
TEST_F(GameDetectionServiceTest,
       RealAudioIdentityIsUniqueAndNeverUsesDisplaySubstring) {
  QProcess a, b;
  a.start("/usr/bin/sleep", {"30"});
  b.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(a.waitForStarted());
  ASSERT_TRUE(b.waitForStarted());
  const auto pa = ProcessCensus::inspect(a.processId()),
             pb = ProcessCensus::inspect(b.processId());
  auto games = ProcessGames::identify({{pa.pid, pa}, {pb.pid, pb}}, {},
                                      {{"sleep", "Fixture"}}, {});
  ASSERT_EQ(games.size(), 2);
  ProcessGames::publish(games);
  EXPECT_EQ(ProcessGames::associate(pa.pid, "sleep"), games[0].id);
  EXPECT_TRUE(ProcessGames::associate(0, "sleep").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(pa.pid, "firefox").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(::getpid(), "sleep").isEmpty());
  EXPECT_TRUE(ProcessGames::associate(0, {}, "Fixture").isEmpty());
  a.kill();
  ASSERT_TRUE(a.waitForFinished());
  EXPECT_TRUE(ProcessGames::associate(pa.pid, "sleep").isEmpty());
  EXPECT_EQ(ProcessGames::associate(0, "sleep"), games[1].id);
  b.kill();
  ASSERT_TRUE(b.waitForFinished());
  ProcessGames::publish({});
}
TEST_F(GameDetectionServiceTest,
       ServicePublishesMultipleGamesWithoutWindowMonitor) {
  GameDetectionService service;
  define(service, "native-one", "One");
  define(service, "native-two", "Two");
  apply(service, {{10, process(10, "/opt/native-one")},
                  {20, process(20, "/opt/native-two")}});
  EXPECT_EQ(service.games().size(), 2);
  EXPECT_EQ(service.currentGameName(), "One");
  ASSERT_TRUE(service.selectInstance(service.games()[1].id));
  EXPECT_EQ(service.currentGameName(), "Two");
  EXPECT_FALSE(service.captureIdentityInfo("Two")
                   .value("detectedGameInstance")
                   .toString()
                   .isEmpty());
  apply(service, {});
  EXPECT_FALSE(service.isGameRunning());
}
TEST_F(GameDetectionServiceTest,
       ManualProcessPinDoesNotRegisterAGameOrSurviveExit) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto pid = child.processId();
  ProcessGames::publish({});
  const auto pin = ProcessGames::pinProcess(pid);
  ASSERT_FALSE(pin.isEmpty());
  EXPECT_TRUE(ProcessGames::snapshot().isEmpty());
  EXPECT_TRUE(ProcessGames::belongsTo(pin, pid, "sleep"));
  EXPECT_FALSE(ProcessGames::belongsTo(pin, pid, "firefox"));
  EXPECT_FALSE(ProcessGames::belongsTo(pin, 0, "sleep"));
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
  EXPECT_FALSE(ProcessGames::belongsTo(pin, pid, "sleep"));
}

TEST_F(GameDetectionServiceTest, LiveCensusDoesNotNeedCompositorOrAudioServer) {
  QProcess child;
  child.start("/usr/bin/sleep", {"30"});
  ASSERT_TRUE(child.waitForStarted());
  const auto desktop = qgetenv("XDG_CURRENT_DESKTOP");
  const auto wayland = qgetenv("WAYLAND_DISPLAY");
  const auto remote = qgetenv("PIPEWIRE_REMOTE");
  struct Restore {
    QByteArray desktop, wayland, remote;
    ~Restore() {
      const auto set = [](const char *key, const QByteArray &value) {
        if (value.isNull())
          qunsetenv(key);
        else
          qputenv(key, value);
      };
      set("XDG_CURRENT_DESKTOP", desktop);
      set("WAYLAND_DISPLAY", wayland);
      set("PIPEWIRE_REMOTE", remote);
    }
  } restore{desktop, wayland, remote};
  qputenv("WAYLAND_DISPLAY", "trophy-test-no-compositor");
  qputenv("PIPEWIRE_REMOTE", "trophy-test-no-audio-server");
  for (const auto *name : {"KDE", "GNOME", "Hyprland"}) {
    qputenv("XDG_CURRENT_DESKTOP", name);
    GameDetectionService service;
    define(service, "sleep", "Native process fixture");
    service.start();
    bool found = false;
    QElapsedTimer timer;
    timer.start();
    while (!found && timer.elapsed() < 4000) {
      QCoreApplication::processEvents();
      for (const auto &g : service.games())
        if (g.processes.contains(child.processId()))
          found = true;
      QThread::msleep(10);
    }
    EXPECT_TRUE(found) << name;
    service.stop();
  }
  child.kill();
  ASSERT_TRUE(child.waitForFinished());
}
TEST_F(GameDetectionServiceTest,
       LegacyEnrollmentCleanupPreservesForeignSettings) {
  QTemporaryDir root;
  ASSERT_TRUE(root.isValid());
  struct Restore {
    QByteArray config = qgetenv("XDG_CONFIG_HOME"),
               data = qgetenv("XDG_DATA_HOME");
    ~Restore() {
      if (config.isNull())
        qunsetenv("XDG_CONFIG_HOME");
      else
        qputenv("XDG_CONFIG_HOME", config);
      if (data.isNull())
        qunsetenv("XDG_DATA_HOME");
      else
        qputenv("XDG_DATA_HOME", data);
      QStandardPaths::setTestModeEnabled(true);
    }
  } restore;
  QStandardPaths::setTestModeEnabled(false);
  qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8());
  qputenv("XDG_DATA_HOME", (root.path() + "/data").toUtf8());
  ASSERT_TRUE(QDir().mkpath(root.path() + "/config/environment.d"));
  ASSERT_TRUE(QDir().mkpath(root.path() + "/data/vulkan/implicit_layer.d"));
  QFile config(root.path() + "/config/environment.d/trophy-gl-shim.conf");
  ASSERT_TRUE(config.open(QIODevice::WriteOnly));
  config.write("# retained "
               "comment\nLD_PRELOAD=/foreign.so:/old/"
               "libtrophy_gl_shim.so\nOTHER=value\n");
  config.close();
  QFile manifest(root.path() +
                 "/data/vulkan/implicit_layer.d/trophy_layer.json");
  ASSERT_TRUE(manifest.open(QIODevice::WriteOnly));
  manifest.write("{}");
  manifest.close();
  removeLegacyDetectionEnrollment();
  ASSERT_TRUE(config.open(QIODevice::ReadOnly));
  const auto remaining = config.readAll();
  EXPECT_TRUE(remaining.contains("/foreign.so"));
  EXPECT_TRUE(remaining.contains("OTHER=value"));
  EXPECT_TRUE(remaining.contains("# retained comment"));
  EXPECT_FALSE(remaining.contains("libtrophy_gl_shim"));
  EXPECT_FALSE(manifest.exists());
}
TEST_F(GameDetectionServiceTest,
       MixedSnapshotCannotAdoptChildOfReusedParentPid) {
  auto replacement = process(10, "/opt/game");
  replacement.start = 1000;
  auto oldChild = process(20, "/opt/audio-helper", 10);
  oldChild.start = 200; // Cannot be the child of this newly reused parent PID.
  const auto games = ProcessGames::identify({{10, replacement}, {20, oldChild}},
                                            {}, {{"game", "Game"}}, {});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games[0].processes.keys(), (QList<qint64>{10}));
}
} // namespace TrophyBackend
