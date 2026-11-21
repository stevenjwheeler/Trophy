/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "detection/LauncherLibrary.h"
#include "detection/ProcessGames.h"
#include "detection/GameWhitelist.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <sqlite3.h>

using namespace TrophyBackend;
class LauncherLibraryTest : public ::testing::Test {
protected:
  QTemporaryDir root;
  QString home = root.path();
  QString config = home + "/config", data = home + "/data";
  QString directory = home + "/Games/A Game";
  QString exe = directory + "/bin/Game.exe";
  void write(const QString &path, const QByteArray &value) {
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(value), value.size());
  }
  void object(const QString &path, const QJsonObject &value) {
    write(path, QJsonDocument(value).toJson());
  }
  void array(const QString &path, const QJsonArray &value) {
    write(path, QJsonDocument(value).toJson());
  }
  void SetUp() override {
    ASSERT_TRUE(root.isValid());
    write(exe, "MZfixture");
  }
  QList<LauncherGame> read() { return LauncherLibrary::read(home, config, data); }
  QJsonObject epic() {
    return {{"title", "An Epic Game"}, {"install_path", directory},
            {"executable", "bin\\Game.exe"}, {"platform", "Windows"}};
  }
  void lutris(const QString &yaml, const QString &runner = "wine",
              bool installed = true, const QString &configId = "a-game") {
    ASSERT_TRUE(QDir().mkpath(data + "/lutris"));
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(QFile::encodeName(data + "/lutris/pga.db").constData(), &db),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE games(name TEXT,runner TEXT,directory TEXT,configpath TEXT,installed INTEGER)",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO games VALUES(?,?,?,?,?)",
                                -1, &statement, nullptr), SQLITE_OK);
    const QStringList fields{"A Lutris Game", runner, directory, configId};
    for (int i = 0; i < fields.size(); ++i)
      sqlite3_bind_text(statement, i + 1, fields[i].toUtf8().constData(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 5, installed);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    write(config + "/lutris/games/a-game.yml", yaml.toUtf8());
  }
  ProcessInfo process(qint64 pid = 70) {
    ProcessInfo p;
    p.pid = pid;
    p.start = pid * 10;
    p.parent = 1;
    p.executable = exe;
    p.arguments = {exe};
    return p;
  }
  LauncherGame game(const QString &name, const QString &prefix = {}) {
    return {0, name, {}, exe, prefix};
  }
  QList<GameInstance> identify(const ProcessCensus::Snapshot &ps,
                               const QList<LauncherGame> &games,
                               const QMap<QString, QString> &rules = {},
                               const QStringList &blocked = {},
                               const QList<GameInstance> &previous = {}) {
    return ProcessGames::identify(ps, games, rules, blocked, previous);
  }
  LauncherGame bootstrap() {
    exe = directory + "/Launcher.exe";
    write(exe, "MZfixture");
    return {0, "A Game", directory, exe, {}, true};
  }
  ProcessCensus::Snapshot privateLaunch(qint64 offset = 0) {
    auto umu = process(30 + offset), wrapper = process(40 + offset),
         runtime = process(50 + offset), launcher = process(70 + offset);
    umu.executable = "/usr/bin/python3";
    umu.arguments = {umu.executable, "/usr/bin/umu-run", exe};
    wrapper.executable = "/runtime/srt-bwrap";
    wrapper.arguments = {wrapper.executable};
    wrapper.parent = umu.pid;
    runtime.executable = "/runtime/pv-adverb";
    runtime.arguments = {runtime.executable, "--subreaper", "--", "/runtime/umu-shim",
                         "/tools/proton", "waitforexitandrun", exe};
    runtime.parent = wrapper.pid;
    launcher.parent = runtime.pid;
    launcher.winePrefix = home + "/prefix";
    return {{umu.pid, umu}, {wrapper.pid, wrapper}, {runtime.pid, runtime},
            {launcher.pid, launcher}};
  }
  ProcessInfo runtimeGame(qint64 offset = 0) {
    auto child = process(71 + offset);
    child.parent = 50 + offset;
    child.executable = directory + "/Retail/Game.exe";
    child.arguments = {child.executable};
    child.winePrefix = home + "/prefix";
    return child;
  }
};

TEST_F(LauncherLibraryTest, MissingLibrariesDoNotCreateFiles) {
  EXPECT_TRUE(read().isEmpty());
  EXPECT_FALSE(QFileInfo(data).exists());
  EXPECT_FALSE(QFileInfo(config).exists());
}
TEST_F(LauncherLibraryTest, SteamUsesTheSameCatalogueWithExtraLibraries) {
  const auto apps = data + "/Steam/steamapps";
  const auto extra = home + "/Extra Library";
  write(apps + "/libraryfolders.vdf",
        ("\"libraryfolders\" { \"1\" { \"path\" \"" + extra + "\" } }").toUtf8());
  write(extra + "/steamapps/appmanifest_42.acf",
        "\"AppState\" { \"appid\" \"42\" \"name\" \"A Steam Game\" \"installdir\" \"A Game\" }");
  const auto games = read();
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().steamId, 42u);
  EXPECT_EQ(games.first().sources, QStringList{"Steam"});
  EXPECT_EQ(games.first().directory, extra + "/steamapps/common/A Game");
  auto p = process();
  p.executable = games.first().directory + "/game";
  p.arguments = {p.executable};
  const auto result = identify({{p.pid, p}}, games);
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().key, "steam_app_42");
}
TEST_F(LauncherLibraryTest, SteamRejectsManifestDirectoryTraversal) {
  write(data + "/Steam/steamapps/appmanifest_42.acf",
        "\"appid\" \"42\" \"name\" \"Bad\" \"installdir\" \"../../Other\"");
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LegendaryAndHeroicEpicUseInstalledExecutables) {
  for (const auto &relative : {"/legendary", "/heroic/legendaryConfig/legendary"})
    object(config + relative + "/installed.json", {{"app-id", epic()}});
  const auto games = read();
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().name, "An Epic Game");
  EXPECT_EQ(games.first().executable, exe);
  EXPECT_EQ(games.first().sources, (QStringList{"Legendary", "Heroic (Epic)"}));
  auto p = process();
  EXPECT_EQ(identify({{p.pid, p}}, games).size(), 1);
  p.executable = home + "/Other/Game.exe";
  p.arguments = {p.executable};
  EXPECT_TRUE(identify({{p.pid, p}}, games).isEmpty());
}
TEST_F(LauncherLibraryTest, UninstalledLibraryMetadataIsNotEvidence) {
  object(config + "/legendary/metadata/app-id.json", epic());
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, InvalidAndIncompleteEpicEntriesAreSkipped) {
  QJsonObject entries;
  for (const auto &flag : {"is_dlc", "is_preloaded", "needs_verification"}) {
    auto entry = epic();
    entry[flag] = true;
    entries[flag] = entry;
  }
  auto missing = epic();
  missing["executable"] = "";
  entries["missing"] = missing;
  auto mac = epic();
  mac["platform"] = "Mac";
  entries["mac"] = mac;
  object(config + "/legendary/installed.json", entries);
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, EpicRejectsTraversalMissingFilesAndMalformedJson) {
  write(home + "/Other/Game.exe", "MZfixture");
  auto entry = epic();
  entry["executable"] = "../../Other/Game.exe";
  object(config + "/legendary/installed.json", {{"bad", entry}});
  EXPECT_TRUE(read().isEmpty());
  entry["executable"] = "missing.exe";
  object(config + "/legendary/installed.json", {{"bad", entry}});
  EXPECT_TRUE(read().isEmpty());
  write(config + "/legendary/installed.json", "{broken");
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, ScriptsAreNotGamePayloads) {
  write(exe, "#!/bin/sh\nexec another-game\n");
  object(config + "/legendary/installed.json", {{"app", epic()}});
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, NativeElfIsRecognized) {
  exe = directory + "/native";
  write(exe, QByteArray("\x7f""ELFfixture"));
  auto entry = epic();
  entry["platform"] = "Linux";
  entry["executable"] = "native";
  object(config + "/legendary/installed.json", {{"app", entry}});
  ASSERT_EQ(read().size(), 1);
  auto p = process();
  EXPECT_EQ(identify({{p.pid, p}}, read()).size(), 1);
}
TEST_F(LauncherLibraryTest, SymlinkedExecutablesMatchCanonicalProcessPaths) {
  const auto target = home + "/Actual Game/Game.exe";
  write(target, "MZfixture");
  ASSERT_TRUE(QFile::remove(exe));
  ASSERT_TRUE(QFile::link(target, exe));
  object(config + "/legendary/installed.json", {{"app", epic()}});
  auto p = process();
  EXPECT_EQ(identify({{p.pid, p}}, read()).size(), 1);
  p.executable = target;
  p.arguments = {target};
  EXPECT_EQ(identify({{p.pid, p}}, read()).size(), 1);
}
TEST_F(LauncherLibraryTest, GogBindsInstalledProductToPrimaryFileTask) {
  object(config + "/heroic/gog_store/installed.json",
         {{"installed", QJsonArray{QJsonObject{{"appName", "123"}, {"install_path", directory}}}}});
  QJsonObject info{{"gameId", "123"}, {"name", "A GOG Game"},
      {"playTasks", QJsonArray{
        QJsonObject{{"isPrimary", false}, {"type", "FileTask"}, {"path", "other.exe"}},
        QJsonObject{{"isPrimary", true}, {"type", "FileTask"},
                    {"workingDir", "bin"}, {"path", "Game.exe"}}}}};
  object(directory + "/goggame-123.info", info);
  ASSERT_EQ(read().size(), 1);
  EXPECT_EQ(read().first().executable, exe);
  EXPECT_EQ(read().first().name, "A GOG Game");
  EXPECT_EQ(read().first().sources, QStringList{"Heroic (GOG)"});
  info["gameId"] = "456";
  object(directory + "/goggame-123.info", info);
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, GogDoesNotTreatUrlTasksAsExecutables) {
  object(config + "/heroic/gog_store/installed.json",
         {{"installed", QJsonArray{QJsonObject{{"appName", "123"}, {"install_path", directory}}}}});
  object(directory + "/goggame-123.info",
         {{"gameId", "123"}, {"name", "A GOG Game"},
          {"playTasks", QJsonArray{QJsonObject{{"isPrimary", true},
              {"type", "URLTask"}, {"path", "bin/Game.exe"}}}}});
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, NileJoinsInstalledIdsToLocalLibraryAndFuel) {
  const auto path = config + "/heroic/nile_config/nile";
  array(path + "/installed.json", {QJsonObject{{"id", "amazon-id"}, {"path", directory}}});
  array(path + "/library.json", {QJsonObject{{"product", QJsonObject{
      {"id", "amazon-id"}, {"title", "An Amazon Game"}}}}});
  object(directory + "/fuel.json", {{"Main", QJsonObject{{"Command", "bin\\Game.exe"}}}});
  ASSERT_EQ(read().size(), 1);
  EXPECT_EQ(read().first().name, "An Amazon Game");
  EXPECT_EQ(read().first().sources, QStringList{"Heroic (Amazon)"});
  EXPECT_EQ(read().first().executable, exe);
  array(path + "/installed.json", {QJsonObject{{"id", "not-in-library"}, {"path", directory}}});
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LutrisWineReadsYamlWithoutExecutingIt) {
  const auto prefix = home + "/Wine Prefix";
  lutris("game:\n  exe: '" + exe + "'\n  prefix: '" + prefix + "'\n");
  const auto games = read();
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().name, "A Lutris Game");
  EXPECT_EQ(games.first().sources, QStringList{"Lutris"});
  EXPECT_EQ(games.first().winePrefix, prefix);
  auto p = process();
  p.winePrefix = home + "/Other Prefix";
  EXPECT_TRUE(identify({{p.pid, p}}, games).isEmpty());
  p.winePrefix = prefix;
  EXPECT_EQ(identify({{p.pid, p}}, games).size(), 1);
}
TEST_F(LauncherLibraryTest, LutrisNativeResolvesRelativeExecutable) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux");
  ASSERT_EQ(read().size(), 1);
  EXPECT_EQ(read().first().executable, exe);
}
TEST_F(LauncherLibraryTest, LutrisUninstalledEntriesAreIgnored) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux", false);
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LutrisEmulatorsAreNotGameExecutables) {
  lutris("game:\n  exe: bin/Game.exe\n", "dosbox");
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LutrisRejectsDuplicateAndStructuredExecutableFields) {
  lutris("game:\n  exe: bin/Game.exe\n  exe: bin/Game.exe\n", "linux");
  EXPECT_TRUE(read().isEmpty());
  write(config + "/lutris/games/a-game.yml", "game:\n  exe: [bin/Game.exe]\n");
  EXPECT_TRUE(read().isEmpty());
  write(config + "/lutris/games/a-game.yml", "game: &loop\n  exe: *loop\n");
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LutrisRejectsConfigTraversalAndCorruptDatabases) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux", true, "../a-game");
  EXPECT_TRUE(read().isEmpty());
  write(data + "/lutris/pga.db", "not a database");
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, LutrisReadLeavesDatabaseAndConfigUnchanged) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux");
  const auto contents = [](const QString &path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
  };
  const auto database = data + "/lutris/pga.db";
  const auto yaml = config + "/lutris/games/a-game.yml";
  const auto beforeDb = contents(database), beforeYaml = contents(yaml);
  EXPECT_EQ(read().size(), 1);
  EXPECT_EQ(contents(database), beforeDb);
  EXPECT_EQ(contents(yaml), beforeYaml);
}
TEST_F(LauncherLibraryTest, LutrisLockedDatabaseDoesNotBlockDiscovery) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux");
  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open(QFile::encodeName(data + "/lutris/pga.db").constData(), &db),
            SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
  EXPECT_TRUE(read().isEmpty());
  EXPECT_EQ(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
  EXPECT_EQ(read().size(), 1);
}
TEST_F(LauncherLibraryTest, OversizedMetadataIsIgnored) {
  write(config + "/legendary/installed.json", QByteArray(4 * 1024 * 1024 + 1, ' '));
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, AmbiguousNamesRequireAnExplicitRule) {
  auto p = process();
  QList<LauncherGame> games{game("One"), game("Two")};
  EXPECT_TRUE(identify({{p.pid, p}}, games).isEmpty());
  const auto key = GameWhitelist::normalizeKey("wine::" + exe);
  const auto result = identify({{p.pid, p}}, games, {{key, "My Name"}});
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().name, "My Name");
}
TEST_F(LauncherLibraryTest, BlockRulesAndConcurrentInstancesArePreserved) {
  const auto p = process(), q = process(71);
  QList<LauncherGame> games{game("A Game")};
  EXPECT_EQ(identify({{p.pid, p}, {q.pid, q}}, games).size(), 2);
  EXPECT_TRUE(identify({{p.pid, p}}, games, {}, {"a game"}).isEmpty());
  EXPECT_TRUE(identify({{p.pid, p}}, games, {},
                      {GameWhitelist::normalizeKey("wine::" + exe)}).isEmpty());
}
TEST_F(LauncherLibraryTest, SharedPrefixAndDirectoryDoNotAdoptUnrelatedProcesses) {
  auto p = process(), q = process(71);
  p.winePrefix = q.winePrefix = home + "/prefix";
  q.executable = directory + "/Other.exe";
  q.arguments = {q.executable};
  const auto result = identify({{p.pid, p}, {q.pid, q}}, {game("A Game", p.winePrefix)});
  ASSERT_EQ(result.size(), 1);
  EXPECT_TRUE(result.first().processes.contains(p.pid));
  EXPECT_FALSE(result.first().processes.contains(q.pid));
}
TEST_F(LauncherLibraryTest, ActualWineLoaderMatchesOnlyItsPayload) {
  auto p = process();
  p.executable = "/usr/bin/wine64";
  p.arguments = {p.executable, exe};
  p.winePrefix = home + "/prefix";
  const auto games = identify({{p.pid, p}}, {game("A Game", p.winePrefix)});
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().primaryPid, p.pid);
  p.arguments = {p.executable, directory + "/Other.exe", exe};
  EXPECT_TRUE(identify({{p.pid, p}}, {game("A Game", p.winePrefix)}).isEmpty());
}
TEST_F(LauncherLibraryTest, LauncherProcessesAreNotSeededByMetadata) {
  for (const auto &name : {"Heroic", "lutris", "GalaxyClient.exe",
                           "EpicGamesLauncher.exe", "Battle.net.exe"}) {
    auto p = process();
    p.executable = directory + "/" + name;
    p.arguments = {p.executable};
    const QList<LauncherGame> games{{0, "Not a game", directory, p.executable, {}, true}};
    EXPECT_TRUE(identify({{p.pid, p}}, games).isEmpty()) << name;
  }
}

TEST_F(LauncherLibraryTest, LegendaryKeepsVerifiedBootstrapAndInstallationScope) {
  const auto expected = bootstrap();
  auto entry = epic();
  entry["executable"] = "Launcher.exe";
  object(config + "/legendary/installed.json", {{"app", entry}});
  const auto catalogue = read();
  ASSERT_EQ(catalogue.size(), 1);
  EXPECT_TRUE(catalogue.first().bootstrap);
  EXPECT_EQ(catalogue.first().directory, directory);
  auto p = process();
  const auto result = identify({{p.pid, p}}, catalogue);
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().name, "An Epic Game");
  EXPECT_EQ(result.first().launchContext.directory, expected.directory);
}
TEST_F(LauncherLibraryTest, UnknownLauncherStillNeedsExactScopedMetadata) {
  auto entry = bootstrap();
  const auto p = process();
  const auto rule = GameWhitelist::normalizeKey("wine::" + exe);
  EXPECT_TRUE(identify({{p.pid, p}}, {}, {{rule, "Not sufficient"}}).isEmpty());
  entry.bootstrap = false;
  EXPECT_TRUE(identify({{p.pid, p}}, {entry}).isEmpty());
  entry.bootstrap = true;
  entry.directory.clear();
  EXPECT_TRUE(identify({{p.pid, p}}, {entry}).isEmpty());
}
TEST_F(LauncherLibraryTest, BootstrapSymlinkCannotEscapeInstallation) {
  bootstrap();
  const auto outside = home + "/Other/Launcher.exe";
  write(outside, "MZfixture");
  ASSERT_TRUE(QFile::remove(exe));
  ASSERT_TRUE(QFile::link(outside, exe));
  auto entry = epic();
  entry["executable"] = "Launcher.exe";
  object(config + "/legendary/installed.json", {{"app", entry}});
  EXPECT_TRUE(read().isEmpty());
}
TEST_F(LauncherLibraryTest, BootstrapMetadataMustHaveUnambiguousNameAndScope) {
  auto entry = bootstrap();
  auto other = entry;
  other.name = "Another Game";
  const auto p = process();
  EXPECT_TRUE(identify({{p.pid, p}}, {entry, other}).isEmpty());
  other = entry;
  other.directory = home + "/Games";
  EXPECT_TRUE(identify({{p.pid, p}}, {entry, other}).isEmpty());
}
TEST_F(LauncherLibraryTest, BootstrapOnlyAdoptsObservedInInstallationDescendants) {
  const auto entry = bootstrap();
  auto rootProcess = process(), child = process(71), sibling = process(72),
       outside = process(73), foreign = process(74), helper = process(75);
  const auto prefix = home + "/prefix";
  rootProcess.winePrefix = prefix;
  for (auto *p : {&child, &sibling, &outside, &foreign, &helper}) {
    p->executable = directory + "/Retail/Game.exe";
    p->arguments = {p->executable};
    p->winePrefix = prefix;
    p->parent = rootProcess.pid;
  }
  sibling.parent = 1;
  outside.executable = home + "/Other/Game.exe";
  outside.arguments = {outside.executable};
  foreign.winePrefix = home + "/other-prefix";
  helper.executable = directory + "/UnityCrashHandler64.exe";
  helper.arguments = {helper.executable};
  const auto result = identify({{rootProcess.pid, rootProcess}, {child.pid, child},
      {sibling.pid, sibling}, {outside.pid, outside}, {foreign.pid, foreign},
      {helper.pid, helper}}, {entry});
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().processes.size(), 2);
  EXPECT_TRUE(result.first().processes.contains(rootProcess.pid));
  EXPECT_TRUE(result.first().processes.contains(child.pid));
  EXPECT_EQ(result.first().primaryPid, child.pid);
}
TEST_F(LauncherLibraryTest, ObservedGameRetainsIdentityAfterBootstrapExit) {
  const auto entry = bootstrap();
  const auto parent = process();
  auto child = process(71);
  child.parent = parent.pid;
  child.executable = directory + "/Retail/Game.exe";
  child.arguments = {child.executable};
  const auto first = identify({{parent.pid, parent}, {child.pid, child}}, {entry});
  ASSERT_EQ(first.size(), 1);
  ASSERT_EQ(first.first().processes.size(), 2);
  child.parent = 1;
  const auto next = identify({{child.pid, child}}, {entry}, {}, {}, first);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, first.first().id);
  EXPECT_EQ(next.first().key, first.first().key);
  EXPECT_EQ(next.first().primaryPid, child.pid);
  EXPECT_EQ(next.first().launchContext.directory, directory);
  EXPECT_TRUE(identify({{child.pid, child}}, {entry}).isEmpty());
  child.start++;
  EXPECT_TRUE(identify({{child.pid, child}}, {entry}, {}, {}, first).isEmpty());
  child.start--;
  child.executable = directory + "/Retail/Other.exe";
  child.arguments = {child.executable};
  EXPECT_TRUE(identify({{child.pid, child}}, {entry}, {}, {}, first).isEmpty());
}
TEST_F(LauncherLibraryTest, BootstrapBlocksApplyBeforeAndAfterHandoff) {
  const auto entry = bootstrap();
  const auto p = process();
  EXPECT_TRUE(identify({{p.pid, p}}, {entry}, {}, {"a game"}).isEmpty());
  EXPECT_TRUE(identify({{p.pid, p}}, {entry}, {},
                      {GameWhitelist::normalizeKey("wine::" + exe)}).isEmpty());
  const auto first = identify({{p.pid, p}}, {entry});
  EXPECT_TRUE(identify({{p.pid, p}}, {entry}, {}, {"a game"}, first).isEmpty());
}
TEST_F(LauncherLibraryTest, ConcurrentBootstrapsDoNotShareInheritedSteamContext) {
  const auto entry = bootstrap();
  auto p = process(), q = process(71), sharedParent = process(60);
  sharedParent.executable = "/usr/bin/umu-run";
  sharedParent.arguments = {sharedParent.executable};
  p.parent = q.parent = sharedParent.pid;
  p.steamId = q.steamId = sharedParent.steamId = 42;
  const auto result = identify({{p.pid, p}, {q.pid, q},
                               {sharedParent.pid, sharedParent}}, {entry});
  ASSERT_EQ(result.size(), 2);
  EXPECT_NE(result[0].id, result[1].id);
}

TEST_F(LauncherLibraryTest, VerifiedPrivateRuntimeConnectsWineSiblings) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  EXPECT_EQ(initial.first().launchContext.runtime.pid, 50);
  const auto child = runtimeGame();
  ps.remove(70);
  ps.insert(child.pid, child);
  const auto next = identify(ps, {entry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().key, initial.first().key);
  EXPECT_EQ(next.first().primaryPid, child.pid);
  EXPECT_EQ(next.first().processes.size(), 1);
  EXPECT_TRUE(next.first().processes.contains(child.pid));
  ps.remove(child.pid);
  EXPECT_TRUE(identify(ps, {entry}, {}, {}, next).isEmpty());
}

TEST_F(LauncherLibraryTest, LiveBootstrapYieldsPrimaryToVerifiedRuntimePayload) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  ASSERT_EQ(initial.first().primaryPid, 70);
  const auto child = runtimeGame();
  ps.insert(child.pid, child); // Launcher stays open alongside the game.
  const auto next = identify(ps, {entry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().primaryPid, child.pid);
  EXPECT_EQ(next.first().processes.size(), 2);
  const auto stable = identify(ps, {entry}, {}, {}, next);
  ASSERT_EQ(stable.size(), 1);
  EXPECT_EQ(stable.first().primaryPid, child.pid);
}
TEST_F(LauncherLibraryTest, PrivateRuntimeDoesNotAdoptPrefixOrInstallationSiblingsAlone) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  auto unrelated = runtimeGame();
  unrelated.parent = 1;
  ps.insert(unrelated.pid, unrelated);
  const auto result = identify(ps, {entry});
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().processes.size(), 1);
  auto outside = runtimeGame(1);
  outside.parent = 50;
  outside.executable = home + "/Other/Game.exe";
  outside.arguments = {outside.executable};
  ps.insert(outside.pid, outside);
  EXPECT_EQ(identify(ps, {entry}).first().processes.size(), 1);
}
TEST_F(LauncherLibraryTest, RuntimeNameWithoutExactLaunchCommandsIsNotEvidence) {
  const auto entry = bootstrap();
  const auto child = runtimeGame();
  for (const auto invalid : {0, 1, 2, 3}) {
    auto ps = privateLaunch();
    if (invalid == 0)
      ps[50].arguments.last() = directory + "/OtherLauncher.exe";
    if (invalid == 1)
      ps[30].arguments.last() = directory + "/OtherLauncher.exe";
    if (invalid == 2)
      ps[40].executable = "/usr/bin/Heroic";
    if (invalid == 3)
      ps[50].arguments[3] = "/runtime/unrecognized-wrapper";
    ps.insert(child.pid, child);
    const auto result = identify(ps, {entry});
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.first().launchContext.runtime.pid, 0);
    EXPECT_EQ(result.first().processes.size(), 1);
  }
}
TEST_F(LauncherLibraryTest, RuntimeReuseOrChangedCommandCannotAdoptNewPayload) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  const auto child = runtimeGame();
  ps.remove(70);
  ps.insert(child.pid, child);
  ps[50].start++;
  EXPECT_TRUE(identify(ps, {entry}, {}, {}, initial).isEmpty());
  ps[50].start--;
  ps[50].arguments.append("--different-command");
  EXPECT_TRUE(identify(ps, {entry}, {}, {}, initial).isEmpty());
}
TEST_F(LauncherLibraryTest, SharedWineBrokerDoesNotBridgeRuntimeSiblings) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  auto broker = process(55), child = runtimeGame();
  broker.executable = "/tools/wineserver";
  broker.arguments = {broker.executable};
  broker.parent = 50;
  child.parent = broker.pid;
  ps.insert(broker.pid, broker);
  ps.insert(child.pid, child);
  const auto result = identify(ps, {entry});
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().processes.size(), 1);
}
TEST_F(LauncherLibraryTest, SeparatePrivateLaunchesRemainIndependentInSamePrefix) {
  const auto entry = bootstrap();
  auto ps = privateLaunch(), other = privateLaunch(100);
  for (const auto &p : other)
    ps.insert(p.pid, p);
  const auto child = runtimeGame(), otherChild = runtimeGame(100);
  ps.insert(child.pid, child);
  ps.insert(otherChild.pid, otherChild);
  const auto result = identify(ps, {entry});
  ASSERT_EQ(result.size(), 2);
  EXPECT_NE(result[0].id, result[1].id);
  EXPECT_EQ(result[0].processes.size(), 2);
  EXPECT_EQ(result[1].processes.size(), 2);
  EXPECT_TRUE(result[0].processes.contains(child.pid));
  EXPECT_FALSE(result[0].processes.contains(otherChild.pid));
  EXPECT_TRUE(result[1].processes.contains(otherChild.pid));
}
TEST_F(LauncherLibraryTest, PrivateContextStillRequiresSamePrefixAndHonorsBlocks) {
  const auto entry = bootstrap();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  auto child = runtimeGame();
  ps.remove(70);
  child.winePrefix = home + "/different-prefix";
  ps.insert(child.pid, child);
  EXPECT_TRUE(identify(ps, {entry}, {}, {}, initial).isEmpty());
  child.winePrefix = home + "/prefix";
  ps[child.pid] = child;
  EXPECT_TRUE(identify(ps, {entry}, {}, {"a game"}, initial).isEmpty());
}

TEST_F(LauncherLibraryTest, OrdinaryCatalogueGameCanOwnRuntimeSibling) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  EXPECT_FALSE(entry.bootstrap);
  EXPECT_EQ(initial.first().launchContext.runtime.pid, 50);
  const auto child = runtimeGame();
  ps.remove(70);
  ps.insert(child.pid, child);
  const auto next = identify(ps, {entry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().key, initial.first().key);
  EXPECT_EQ(next.first().primaryPid, child.pid);
  ps.remove(child.pid);
  EXPECT_TRUE(identify(ps, {entry}, {}, {}, next).isEmpty());
}
TEST_F(LauncherLibraryTest, OrdinaryCatalogueSiblingsWithOwnEntriesUseOneIdentity) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto childEntry = entry;
  const auto child = runtimeGame();
  childEntry.executable = child.payload();
  auto ps = privateLaunch();
  ps.insert(child.pid, child);
  const auto initial = identify(ps, {entry, childEntry});
  ASSERT_EQ(initial.size(), 1);
  EXPECT_EQ(initial.first().processes.size(), 2);
  EXPECT_EQ(initial.first().launchContext.runtime.pid, 50);
  ps.remove(70);
  const auto next = identify(ps, {entry, childEntry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().key, initial.first().key);
}
TEST_F(LauncherLibraryTest, PreviouslyKnownSiblingKeepsIdentityWithoutSeedOverlap) {
  auto entry = game("A Game");
  entry.directory = directory;
  const auto child = runtimeGame();
  auto childEntry = entry;
  childEntry.executable = child.payload();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry, childEntry});
  ASSERT_EQ(initial.size(), 1);
  ps.remove(70);
  ps.insert(child.pid, child);
  const auto next = identify(ps, {entry, childEntry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().key, initial.first().key);
}
TEST_F(LauncherLibraryTest, ExplicitSiblingRuleIsNotOverriddenByRuntimeContext) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto ps = privateLaunch();
  const auto child = runtimeGame();
  ps.insert(child.pid, child);
  const auto key = GameWhitelist::normalizeKey("wine:" + child.winePrefix + ":" + child.payload());
  const auto result = identify(ps, {entry}, {{key, "Separately Tracked"}});
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0].name, "A Game");
  EXPECT_EQ(result[1].name, "Separately Tracked");
  EXPECT_EQ(result[0].processes.size(), 1);
  EXPECT_EQ(result[1].processes.size(), 1);
}
TEST_F(LauncherLibraryTest, SteamCatalogueGamesUseTheSamePrivateContextMechanism) {
  const LauncherGame entry{42, "A Steam Game", directory};
  auto ps = privateLaunch();
  for (auto it = ps.begin(); it != ps.end(); ++it)
    it->steamId = 42;
  auto child = runtimeGame();
  child.steamId = 42;
  ps.insert(child.pid, child);
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  EXPECT_EQ(initial.first().key, "steam_app_42");
  EXPECT_EQ(initial.first().launchContext.runtime.pid, 50);
  EXPECT_EQ(initial.first().processes.size(), 2);
  ps.remove(70);
  const auto next = identify(ps, {entry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().id, initial.first().id);
  EXPECT_EQ(next.first().primaryPid, child.pid);
}
TEST_F(LauncherLibraryTest, UserRuleNeedsIndependentInstallationEvidenceForSiblings) {
  auto ps = privateLaunch();
  const auto child = runtimeGame();
  ps.insert(child.pid, child);
  const auto seed = ps.value(70);
  const auto key = GameWhitelist::normalizeKey("wine:" + seed.winePrefix + ":" + seed.payload());
  const auto manual = identify(ps, {}, {{key, "My Game"}});
  ASSERT_EQ(manual.size(), 1);
  EXPECT_FALSE(manual.first().launchContext.hasScope());
  EXPECT_EQ(manual.first().processes.size(), 1);
  auto entry = game("Catalogue Name");
  entry.directory = directory;
  const auto corroborated = identify(ps, {entry}, {{key, "My Game"}});
  ASSERT_EQ(corroborated.size(), 1);
  EXPECT_EQ(corroborated.first().name, "My Game");
  EXPECT_EQ(corroborated.first().processes.size(), 2);
}
TEST_F(LauncherLibraryTest, AmbiguousCatalogueScopeCannotEnableSiblingOwnership) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto conflicting = entry;
  conflicting.directory = home + "/Games";
  auto ps = privateLaunch();
  const auto child = runtimeGame();
  ps.insert(child.pid, child);
  const auto result = identify(ps, {entry, conflicting});
  ASSERT_EQ(result.size(), 1);
  EXPECT_FALSE(result.first().launchContext.hasScope());
  EXPECT_EQ(result.first().processes.size(), 1);
}
TEST_F(LauncherLibraryTest, OrdinaryConcurrentLaunchContextsRemainIndependent) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto ps = privateLaunch();
  const auto other = privateLaunch(100);
  for (const auto &p : other)
    ps.insert(p.pid, p);
  const auto child = runtimeGame(), secondChild = runtimeGame(100);
  ps.insert(child.pid, child);
  ps.insert(secondChild.pid, secondChild);
  const auto result = identify(ps, {entry});
  ASSERT_EQ(result.size(), 2);
  EXPECT_NE(result[0].id, result[1].id);
  EXPECT_EQ(result[0].processes.size(), 2);
  EXPECT_EQ(result[1].processes.size(), 2);
}
TEST_F(LauncherLibraryTest, RecognizedSiblingCannotEscapeContextBlock) {
  auto entry = game("A Game");
  entry.directory = directory;
  const auto child = runtimeGame();
  auto childEntry = entry;
  childEntry.executable = child.payload();
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry, childEntry});
  ASSERT_EQ(initial.size(), 1);
  ps.remove(70);
  ps.insert(child.pid, child);
  EXPECT_TRUE(identify(ps, {entry, childEntry}, {}, {"a game"}, initial).isEmpty());
  EXPECT_TRUE(identify(ps, {entry, childEntry}, {},
                      {initial.first().key}, initial).isEmpty());
}
TEST_F(LauncherLibraryTest, InheritedSteamVariablesDoNotGroupNonSteamCatalogueGames) {
  auto entry = game("A Game");
  entry.directory = directory;
  auto first = process(), second = process(71), shared = process(30);
  shared.executable = "/usr/bin/umu-run";
  shared.arguments = {shared.executable};
  first.parent = second.parent = shared.pid;
  first.steamId = second.steamId = shared.steamId = 42;
  const auto result = identify({{first.pid, first}, {second.pid, second},
                                {shared.pid, shared}}, {entry});
  ASSERT_EQ(result.size(), 2);
  EXPECT_NE(result[0].id, result[1].id);
}
TEST_F(LauncherLibraryTest, BootstrapExceptionCannotBroadenToAnotherCatalogueScope) {
  const auto entry = bootstrap();
  const LauncherGame broad{42, "A Game", home + "/Games"};
  const auto result = identify(privateLaunch(), {entry, broad});
  ASSERT_EQ(result.size(), 1);
  EXPECT_EQ(result.first().launchContext.directory, directory);
}
TEST_F(LauncherLibraryTest, DetectionSourcesDistinguishMetadataFromUserRules) {
  auto entry = game("A Game");
  entry.sources = {"Lutris"};
  const auto p = process();
  const auto key = GameWhitelist::normalizeKey("wine::" + p.payload());
  const auto automatic = identify({{p.pid, p}}, {entry});
  ASSERT_EQ(automatic.size(), 1);
  EXPECT_EQ(automatic.first().detectionSources, QStringList{"Lutris"});
  const auto custom = identify({{p.pid, p}}, {}, {{key, "My Game"}});
  ASSERT_EQ(custom.size(), 1);
  EXPECT_EQ(custom.first().detectionSources, QStringList{"user rule"});
  const auto combined = identify({{p.pid, p}}, {entry}, {{key, "My Game"}});
  ASSERT_EQ(combined.size(), 1);
  EXPECT_EQ(combined.first().name, "My Game");
  EXPECT_EQ(combined.first().detectionSources, (QStringList{"Lutris", "user rule"}));
}
TEST_F(LauncherLibraryTest, DetectionSourceSurvivesPrivateRuntimeHandoff) {
  auto entry = bootstrap();
  entry.sources = {"Heroic (Epic)"};
  auto ps = privateLaunch();
  const auto initial = identify(ps, {entry});
  ASSERT_EQ(initial.size(), 1);
  EXPECT_EQ(initial.first().detectionSources, QStringList{"Heroic (Epic)"});
  const auto child = runtimeGame();
  ps.remove(70);
  ps.insert(child.pid, child);
  const auto next = identify(ps, {entry}, {}, {}, initial);
  ASSERT_EQ(next.size(), 1);
  EXPECT_EQ(next.first().detectionSources, initial.first().detectionSources);
}
