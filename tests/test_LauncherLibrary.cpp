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
#include <QElapsedTimer>
#include <QtEndian>
#include <gtest/gtest.h>
#include <sqlite3.h>
#include <algorithm>
#include <sys/stat.h>
#include <zlib.h>

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
  void heroicSteam(const QString &title = "Heroic Steam Title", const QString &id = "12345",
                   const QString &installDirectory = {}) {
    const auto path = installDirectory.isEmpty() ? directory : installDirectory;
    object(config + "/heroic/steam_store/installed.json",
           {{"installed", QJsonArray{QJsonObject{{"appName", id}, {"install_path", path},
                                               {"platform", "windows"}, {"is_dlc", false}}}}});
    object(config + "/heroic/store_cache/steam_library.json",
           {{"games", QJsonArray{QJsonObject{{"app_name", id}, {"title", title}}}}});
  }
  void heroicItch(const QString &title = "Heroic itch Title", const QString &id = "123",
                  const QString &installDirectory = {}, const QString &payload = {}) {
    object(config + "/heroic/store_cache/itchio_library.json",
           {{"library", QJsonArray{QJsonObject{{"app_name", "itchio-" + id}, {"title", title},
               {"is_installed", true}, {"install", QJsonObject{
                   {"install_path", installDirectory.isEmpty() ? directory : installDirectory},
                   {"executable", payload.isEmpty() ? exe : payload}, {"platform", "windows"}}}}}}});
  }
  QJsonObject verdict(const QString &path = "bin/Game.exe", const QString &flavor = "windows") {
    return {{"basePath", "/stale/path"}, {"candidates", QJsonArray{
        QJsonObject{{"path", path}, {"flavor", flavor}}}}};
  }
  void pe(const QString &path, quint16 flags = 0x0002, quint16 subsystem = 2,
          quint16 magic = 0x020b) {
    QByteArray header(512, '\0');
    header.replace(0, 2, "MZ");
    qToLittleEndian<quint32>(128, header.data() + 0x3c);
    header.replace(128, 4, QByteArray("PE\0\0", 4));
    qToLittleEndian<quint16>(magic == 0x010b ? 0x014c : 0x8664, header.data() + 132);
    qToLittleEndian<quint16>(magic == 0x010b ? 224 : 240, header.data() + 148);
    qToLittleEndian<quint16>(flags, header.data() + 150);
    qToLittleEndian<quint16>(magic, header.data() + 152);
    qToLittleEndian<quint16>(subsystem, header.data() + 220);
    write(path, header);
  }
  QByteArray gzip(const QByteArray &input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
      ADD_FAILURE() << "Cannot initialize gzip fixture";
      return {};
    }
    QByteArray output(compressBound(input.size()) + 64, '\0');
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const auto result = deflate(&stream, Z_FINISH);
    const auto length = stream.total_out;
    deflateEnd(&stream);
    EXPECT_EQ(result, Z_STREAM_END);
    output.resize(length);
    return output;
  }
  void receipt(const QJsonArray &files, const QString &id = "123",
               const QString &classification = "game") {
    write(directory + "/.itch/receipt.json.gz", gzip(QJsonDocument(QJsonObject{
        {"game", QJsonObject{{"id", id.toLongLong()}, {"classification", classification}}},
        {"files", files}, {"installerName", "archive"}}).toJson()));
  }
  void itch(const QJsonObject &launch, const QString &title = "Native itch Title",
            const QString &id = "123", const QString &custom = {},
            const QString &locationRoot = {}, const QString &classification = "game") {
    const auto root = locationRoot.isEmpty() ? config + "/itch" : locationRoot;
    ASSERT_TRUE(QDir().mkpath(root + "/db"));
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open(QFile::encodeName(root + "/db/butler.db").constData(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS games(id INTEGER PRIMARY KEY,title TEXT,classification TEXT);"
        "CREATE TABLE IF NOT EXISTS install_locations(id TEXT PRIMARY KEY,path TEXT);"
        "CREATE TABLE IF NOT EXISTS caves(id TEXT,game_id INTEGER,install_location_id TEXT,"
        "install_folder_name TEXT,custom_install_folder TEXT,verdict TEXT,installed_at TEXT,morphing INTEGER);",
        nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_stmt *statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO games VALUES(?,?,?)",
                                -1, &statement, nullptr), SQLITE_OK);
    const QStringList fields{id, title, classification};
    for (int i = 0; i < fields.size(); ++i)
      sqlite3_bind_text(statement, i + 1, fields[i].toUtf8().constData(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO install_locations VALUES('main',?)",
                                -1, &statement, nullptr), SQLITE_OK);
    const auto parent = QFileInfo(directory).absolutePath().toUtf8();
    sqlite3_bind_text(statement, 1, parent.constData(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO caves VALUES(?,?,'main',?,?,?,'2026-11-26',0)",
                                -1, &statement, nullptr), SQLITE_OK);
    const QStringList caveFields{"cave-" + id, id, QFileInfo(directory).fileName(), custom,
        QString::fromUtf8(QJsonDocument(launch).toJson())};
    for (int i = 0; i < caveFields.size(); ++i)
      sqlite3_bind_text(statement, i + 1, caveFields[i].toUtf8().constData(), -1, SQLITE_TRANSIENT);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_DONE);
    sqlite3_finalize(statement);
    ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
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

TEST_F(LauncherLibraryTest, DiscoveryAlwaysListsSupportedLaunchers) {
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.launchers.size(), 7);
  for (const auto &value : result.launchers) {
    const auto launcher = value.toMap();
    EXPECT_FALSE(launcher.value("detected").toBool());
    EXPECT_TRUE(launcher.value("automatic").toBool());
    EXPECT_EQ(launcher.value("count").toInt(), 0);
    EXPECT_TRUE(launcher.value("games").toList().isEmpty());
  }
}

TEST_F(LauncherLibraryTest, HeroicSteamSupplementsManifestsWithoutDuplicatingRenamedTitles) {
  write(home + "/.steam/steam/steamapps/appmanifest_12345.acf",
        "\"appid\" \"12345\" \"name\" \"Steam Title\" \"installdir\" \"Shared\"");
  const auto steamDirectory = home + "/.steam/steam/steamapps/common/Shared";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(steamDirectory).absolutePath()));
  ASSERT_TRUE(QFile::link(directory, steamDirectory));
  heroicSteam("A Different Heroic Title", "12345", directory);
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 2); // Two path spellings, one physical installation.
  EXPECT_EQ(result.games.first().steamId, 12345u);
  EXPECT_EQ(result.games.first().name, "Steam Title");
  EXPECT_EQ(result.games.first().sources, (QStringList{"Steam", "Heroic (Steam)"}));
  EXPECT_EQ(result.launchers[0].toMap().value("count").toInt(), 1);
  EXPECT_EQ(result.launchers[1].toMap().value("count").toInt(), 1);
  EXPECT_EQ(result.games[0].installation, result.games[1].installation);
  auto p = process();
  p.steamId = 12345;
  const auto detected = identify({{p.pid, p}}, result.games);
  ASSERT_EQ(detected.size(), 1);
  EXPECT_EQ(detected.first().detectionSources, (QStringList{"Steam", "Heroic (Steam)"}));
  EXPECT_TRUE(result.watchPaths.contains(config + "/heroic/store_cache/steam_library.json"));
}

TEST_F(LauncherLibraryTest, HeroicSteamWorksWithoutManifestsButKeepsSeparateInstallations) {
  heroicSteam();
  auto games = read();
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().sources, QStringList{"Heroic (Steam)"});
  auto p = process();
  p.steamId = 12345;
  ASSERT_EQ(identify({{p.pid, p}}, games).size(), 1);
  const auto steamRoot = home + "/.steam/steam/steamapps";
  ASSERT_TRUE(QDir().mkpath(steamRoot + "/common/Other"));
  write(steamRoot + "/appmanifest_12345.acf",
        "\"appid\" \"12345\" \"name\" \"Steam Title\" \"installdir\" \"Other\"");
  games = read();
  ASSERT_EQ(games.size(), 2);
  EXPECT_NE(games[0].installation, games[1].installation);
}

TEST_F(LauncherLibraryTest, HeroicSteamRejectsInvalidIdsMissingInstallsAndSupportTools) {
  heroicSteam("Invalid", "4294967296");
  EXPECT_TRUE(read().isEmpty());
  heroicSteam("Missing", "12345", home + "/missing");
  EXPECT_TRUE(read().isEmpty());
  heroicSteam("Proton", "1493710");
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_TRUE(result.games.first().runtimeTool);
  EXPECT_EQ(result.launchers[1].toMap().value("count").toInt(), 0);
}

TEST_F(LauncherLibraryTest, HeroicAndNativeItchMergeSameInstallDespiteDifferentTitles) {
  heroicItch();
  itch(verdict());
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  const auto &game = result.games.first();
  EXPECT_EQ(game.productId, "itch:123");
  EXPECT_EQ(game.sources, (QStringList{"Heroic (itch.io)", "itch.io"}));
  EXPECT_EQ(game.executable, exe);
  EXPECT_EQ(result.launchers[1].toMap().value("count").toInt(), 1);
  EXPECT_EQ(result.launchers[6].toMap().value("count").toInt(), 1);
  auto p = process();
  p.winePrefix = home + "/Wine";
  const auto detected = identify({{p.pid, p}}, result.games);
  ASSERT_EQ(detected.size(), 1);
  EXPECT_EQ(detected.first().detectionSources, game.sources);
  p.executable = directory + "/Unrelated.exe";
  p.arguments = {p.executable};
  EXPECT_TRUE(identify({{p.pid, p}}, result.games).isEmpty());
}

TEST_F(LauncherLibraryTest, ItchUsesCurrentLocationAndExactRecordedLinuxBinary) {
  const auto native = directory + "/NativeGame";
  write(native, QByteArray("\x7f""ELFfixture", 11));
  itch(verdict("NativeGame", "linux"));
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().executable, native);
  EXPECT_EQ(result.games.first().sources, QStringList{"itch.io"});
  EXPECT_TRUE(result.watchPaths.contains(config + "/itch/db/butler.db"));
  const auto p = process();
  EXPECT_TRUE(identify({{p.pid, p}}, result.games).isEmpty());
}

TEST_F(LauncherLibraryTest, ItchSupportsCustomInstallFoldersAndFlatpakConfiguration) {
  const auto other = home + "/Other install";
  write(other + "/Game.exe", "MZfixture");
  const auto flatpak = home + "/.var/app/io.itch.itch/config/itch";
  itch(verdict("Game.exe"), "Custom", "321", other, flatpak);
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().executable, other + "/Game.exe");
  EXPECT_TRUE(result.launchers[6].toMap().value("detected").toBool());
  EXPECT_TRUE(result.watchPaths.contains(flatpak + "/db/butler.db"));
}

TEST_F(LauncherLibraryTest, ItchDoesNotMergeDifferentProductsOrDifferentInstallationsByTitle) {
  heroicItch("Same Title");
  itch(verdict(), "Same Title", "456");
  EXPECT_EQ(read().size(), 2); // Same path, conflicting product identity.
  const auto other = home + "/Other";
  write(other + "/Game.exe", "MZfixture");
  itch(verdict("Game.exe"), "Same Title", "123", other);
  const auto games = read();
  ASSERT_EQ(games.size(), 3);
  EXPECT_NE(games[0].installation, games[2].installation);
}

TEST_F(LauncherLibraryTest, ItchRejectsNonGamesScriptsBrowsersInstallersAndEscapingCandidates) {
  const auto outside = home + "/Outside.exe";
  write(outside, "MZfixture");
  write(directory + "/start.sh", "#!/bin/sh\nexec something");
  ASSERT_TRUE(QFile::link(outside, directory + "/escaped.exe"));
  const QJsonArray candidates{
      QJsonObject{{"path", "start.sh"}, {"flavor", "script"}},
      QJsonObject{{"path", "bin/Game.exe"}, {"flavor", "html"}},
      QJsonObject{{"path", "../../Outside.exe"}, {"flavor", "windows"}},
      QJsonObject{{"path", "escaped.exe"}, {"flavor", "windows"}},
      QJsonObject{{"path", "bin/Game.exe"}, {"flavor", "windows"},
                  {"windowsInfo", QJsonObject{{"installerType", "inno"}}}},
      QJsonObject{{"path", "bin/Game.exe"}, {"flavor", "linux"}}};
  itch({{"candidates", candidates}});
  EXPECT_TRUE(read().isEmpty());
  itch(verdict(), "Editor", "456", {}, {}, "tool");
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchCustomLocationsRespectAutomaticToggleAndNeverCreateDatabases) {
  EXPECT_TRUE(read().isEmpty());
  EXPECT_FALSE(QFileInfo::exists(config + "/itch/db/butler.db"));
  itch(verdict(), "Custom", "123", {}, home + "/custom-itch");
  itch(verdict(), "Automatic");
  const auto result = LauncherLibrary::discover(home, config, data,
      {{"itch", QVariantMap{{"automatic", false}, {"paths", QStringList{"~/custom-itch"}}}}});
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().name, "Custom");
  EXPECT_EQ(result.launchers[6].toMap().value("locations").toList().size(), 1);
  EXPECT_FALSE(result.launchers[6].toMap().value("automatic").toBool());
}

TEST_F(LauncherLibraryTest, ItchInvalidSchemaIsReportedWithoutChangingDatabase) {
  write(config + "/itch/db/butler.db", "Not a database");
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.games.isEmpty());
  const auto locations = result.launchers[6].toMap().value("locations").toList();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].toMap().value("status").toString(), "Unreadable or invalid metadata");
  QFile file(config + "/itch/db/butler.db");
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_EQ(file.readAll(), "Not a database");
}

TEST_F(LauncherLibraryTest, ItchReadOnlyWalSnapshotAndLockBudgetAndInstallState) {
  itch(verdict());
  const auto path = config + "/itch/db/butler.db";
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto before = file.readAll();
  file.close();
  ASSERT_EQ(read().size(), 1);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_EQ(file.readAll(), before);
  file.close();
  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open(QFile::encodeName(path).constData(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "PRAGMA journal_mode=WAL;"
      "UPDATE games SET title='Updated in WAL';BEGIN IMMEDIATE;"
      "UPDATE games SET title='Uncommitted';", nullptr, nullptr, nullptr), SQLITE_OK);
  const auto snapshot = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(snapshot.games.size(), 1);
  EXPECT_EQ(snapshot.games.first().name, "Updated in WAL");
  EXPECT_TRUE(snapshot.watchPaths.contains(path + "-wal"));
  ASSERT_EQ(sqlite3_exec(db, "ROLLBACK;UPDATE caves SET morphing=1",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  EXPECT_TRUE(read().isEmpty());
  ASSERT_EQ(sqlite3_exec(db, "UPDATE caves SET morphing=0,installed_at=NULL",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  EXPECT_TRUE(read().isEmpty());
  ASSERT_EQ(sqlite3_exec(db, "PRAGMA journal_mode=DELETE;BEGIN EXCLUSIVE",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  QElapsedTimer timer;
  timer.start();
  EXPECT_TRUE(read().isEmpty());
  EXPECT_LT(timer.elapsed(), 500);
  ASSERT_EQ(sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
}

TEST_F(LauncherLibraryTest, HeroicItchOnlyAcceptsInstalledExecutableRecords) {
  const auto file = config + "/heroic/store_cache/itchio_library.json";
  heroicItch();
  ASSERT_EQ(read().size(), 1);
  QFile input(file);
  ASSERT_TRUE(input.open(QIODevice::ReadOnly));
  const auto valid = QJsonDocument::fromJson(input.readAll()).object().value("library").toArray()[0].toObject();
  input.close();
  for (const auto &field : QStringList{"is_installed", "app_name", "install", "classification"}) {
    auto entry = valid;
    entry[field] = field == "is_installed" ? QJsonValue(false)
        : field == "app_name" ? QJsonValue("itchio-0")
        : field == "classification" ? QJsonValue("tool") : QJsonValue(QJsonObject{});
    object(file, {{"library", QJsonArray{entry}}});
    EXPECT_TRUE(read().isEmpty()) << field.toStdString();
  }
  for (const auto &field : QStringList{"install_path", "executable", "platform"}) {
    auto entry = valid;
    auto install = entry.value("install").toObject();
    install[field] = field == "executable" ? "https://example.invalid/game"
        : field == "platform" ? "osx" : "/missing";
    entry["install"] = install;
    object(file, {{"library", QJsonArray{entry}}});
    EXPECT_TRUE(read().isEmpty()) << field.toStdString();
  }
  object(file, {{"library", "wrong shape"}});
  EXPECT_TRUE(read().isEmpty());
  write(file, "{bad json");
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchNeverOpensNamedPipesAsExecutablePayloads) {
  const auto pipe = directory + "/pipe.exe";
  ASSERT_EQ(::mkfifo(QFile::encodeName(pipe).constData(), 0600), 0);
  itch(verdict("pipe.exe"));
  heroicItch("Pipe", "123", directory, pipe);
  QElapsedTimer timer;
  timer.start();
  EXPECT_TRUE(read().isEmpty());
  EXPECT_LT(timer.elapsed(), 500);
}

TEST_F(LauncherLibraryTest, LegendaryAndHeroicMergeOnlyMatchingProductsAndPhysicalInstalls) {
  auto first = epic();
  auto second = first;
  second["title"] = "A Different Legendary Title";
  object(config + "/heroic/legendaryConfig/legendary/installed.json", {{"same-id", first}});
  object(config + "/legendary/installed.json", {{"same-id", second}});
  auto games = read();
  ASSERT_EQ(games.size(), 1);
  EXPECT_EQ(games.first().sources, (QStringList{"Legendary", "Heroic (Epic)"}));
  object(config + "/legendary/installed.json", {{"different-id", second}});
  games = read();
  EXPECT_EQ(games.size(), 2);
}

TEST_F(LauncherLibraryTest, CanonicalPrefixGroupingRetainsEveryExactPrefixAlias) {
  ASSERT_TRUE(QDir().mkpath(home + "/prefix"));
  ASSERT_TRUE(QDir().mkpath(home + "/different-prefix"));
  ASSERT_TRUE(QFile::link(home + "/prefix", home + "/prefix-alias"));
  QJsonArray entries;
  for (const auto &prefix : QStringList{home + "/prefix", home + "/prefix-alias",
                                       home + "/different-prefix"})
    entries.append(QJsonObject{{"title", "Prefix Game"}, {"path", exe},
                                {"prefix", prefix}, {"runner", "Default"}});
  array(data + "/faugus-launcher/games.json", entries);
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 3);
  EXPECT_EQ(result.launchers[5].toMap().value("count").toInt(), 2);
  for (const auto &prefix : QStringList{home + "/prefix", home + "/prefix-alias",
                                       home + "/different-prefix"}) {
    auto p = process();
    p.winePrefix = prefix;
    ASSERT_EQ(identify({{p.pid, p}}, result.games).size(), 1);
  }
}

TEST_F(LauncherLibraryTest, ItchNullHostVerdictUsesReceiptWindowsGameForProton) {
  const auto gamePath = directory + "/HiddenPaws/Hidden Paws.exe";
  pe(gamePath);
  pe(directory + "/HiddenPaws/UnityCrashHandler64.exe");
  itch({{"candidates", QJsonValue(QJsonValue::Null)}}, "Hidden Paws");
  receipt({"HiddenPaws/Hidden Paws.exe", "HiddenPaws/UnityCrashHandler64.exe",
           "HiddenPaws/UnityPlayer.dll"});
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().name, "Hidden Paws");
  EXPECT_EQ(result.games.first().executable, gamePath);
  EXPECT_EQ(result.games.first().productId, "itch:123");
  EXPECT_EQ(result.games.first().sources, QStringList{"itch.io"});
  EXPECT_TRUE(result.watchPaths.contains(directory + "/.itch/receipt.json.gz"));
  auto p = process();
  p.executable = "/Proton/files/bin/wine64-preloader";
  p.arguments = {p.executable, gamePath};
  p.winePrefix = home + "/Proton prefix";
  const auto identified = identify({{p.pid, p}}, result.games);
  ASSERT_EQ(identified.size(), 1);
  EXPECT_EQ(identified.first().name, "Hidden Paws");
  EXPECT_TRUE(identified.first().key.startsWith("wine:"));
  EXPECT_EQ(identified.first().detectionSources, QStringList{"itch.io"});
}

TEST_F(LauncherLibraryTest, ItchReceiptComplementsNativeVerdictWithoutDuplicatingPayloads) {
  pe(exe);
  const auto native = directory + "/NativeGame";
  write(native, QByteArray("\x7f""ELFfixture", 11));
  itch(verdict("NativeGame", "linux"));
  receipt({"bin/Game.exe", "bin/Game.exe"});
  auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 2);
  EXPECT_EQ(result.games[0].installation, result.games[1].installation);
  EXPECT_EQ(result.launchers[6].toMap().value("count").toInt(), 1);
  // Re-publish a verdict which already includes the same Windows payload.
  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open(QFile::encodeName(config + "/itch/db/butler.db").constData(), &db), SQLITE_OK);
  const auto sql = "UPDATE caves SET verdict='" +
      QJsonDocument(verdict()).toJson(QJsonDocument::Compact) + "'";
  ASSERT_EQ(sqlite3_exec(db, sql.constData(), nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
  result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().executable, exe);
}

TEST_F(LauncherLibraryTest, ItchReceiptMustBelongToInstalledProductAndBeAGame) {
  pe(exe);
  itch({});
  receipt({"bin/Game.exe"}, "456");
  EXPECT_TRUE(read().isEmpty());
  receipt({"bin/Game.exe"}, "123", "tool");
  EXPECT_TRUE(read().isEmpty());
  receipt({"bin/Game.exe"});
  ASSERT_EQ(read().size(), 1);
  const auto path = directory + "/.itch/receipt.json.gz";
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto before = file.readAll();
  file.close();
  ASSERT_EQ(read().size(), 1);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  EXPECT_EQ(file.readAll(), before);
}

TEST_F(LauncherLibraryTest, ItchReceiptRejectsHelpersInstallersDllsAndInvalidPeImages) {
  itch({});
  const QStringList rejected{"setup.exe", "unins000.exe", "UnityCrashHandler64.exe",
      "Game.dll", "RenamedDll.exe", "Driver.exe", "Truncated.exe", "NotPe.exe", "Forged.exe"};
  for (const auto &name : rejected)
    pe(directory + "/" + name);
  pe(directory + "/RenamedDll.exe", 0x2002);
  pe(directory + "/Driver.exe", 0x0002, 1);
  write(directory + "/Truncated.exe", "MZ");
  write(directory + "/NotPe.exe", QByteArray(512, 'x'));
  QByteArray forged(512, '\0');
  forged.replace(0, 2, "MZ");
  qToLittleEndian<quint32>(0xfffffff0, forged.data() + 0x3c);
  write(directory + "/Forged.exe", forged);
  QJsonArray files;
  for (const auto &name : rejected)
    files.append(name);
  receipt(files);
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchReceiptCannotReintroduceExplicitlyRejectedInstaller) {
  pe(exe);
  auto candidate = QJsonObject{{"path", "bin/Game.exe"}, {"flavor", "windows"},
      {"windowsInfo", QJsonObject{{"installerType", "inno"}}}};
  itch({{"candidates", QJsonArray{candidate}}});
  receipt({"bin/Game.exe"});
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchReceiptRejectsEscapesForeignSymlinksAndSpecialFiles) {
  itch({});
  const auto outside = home + "/Outside.exe";
  pe(outside);
  ASSERT_TRUE(QFile::link(outside, directory + "/escape.exe"));
  ASSERT_EQ(::mkfifo(QFile::encodeName(directory + "/pipe.exe").constData(), 0600), 0);
  receipt({"../../Outside.exe", outside, "escape.exe", "pipe.exe"});
  EXPECT_TRUE(read().isEmpty());
  pe(exe);
  receipt({"bin/Game.exe"});
  const auto path = directory + "/.itch/receipt.json.gz";
  ASSERT_TRUE(QFile::rename(path, home + "/foreign-receipt.gz"));
  ASSERT_TRUE(QFile::link(home + "/foreign-receipt.gz", path));
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchReceiptChecksGzipIntegrityAndDecodedSizeBeforeParsing) {
  pe(exe);
  itch({});
  receipt({"bin/Game.exe"});
  const auto path = directory + "/.itch/receipt.json.gz";
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::ReadOnly));
  const auto valid = file.readAll();
  file.close();
  auto corrupt = valid;
  corrupt[corrupt.size() - 8] = static_cast<char>(corrupt[corrupt.size() - 8] ^ 0x40);
  for (const auto &input : QList<QByteArray>{valid.left(valid.size() / 2), corrupt,
       valid + valid, QByteArray("not gzip"), gzip(QByteArray(17 * 1024 * 1024, ' '))}) {
    write(path, input);
    EXPECT_TRUE(read().isEmpty());
  }
  write(path, valid);
  ASSERT_EQ(read().size(), 1);
}

TEST_F(LauncherLibraryTest, ItchReceiptWatchRepairsMissingReceiptWithoutDatabaseChanges) {
  pe(exe);
  itch({});
  auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.games.isEmpty());
  EXPECT_TRUE(result.watchPaths.contains(directory));
  receipt({"bin/Game.exe"});
  result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_TRUE(result.watchPaths.contains(directory + "/.itch/receipt.json.gz"));
}

TEST_F(LauncherLibraryTest, ItchReceiptSupportsBothPe32AndPe64ConsoleGames) {
  itch({});
  pe(directory + "/Game32.exe", 0x0002, 3, 0x010b);
  pe(directory + "/Game64.exe", 0x0002, 3, 0x020b);
  receipt({"Game32.exe", "Game64.exe"});
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 2);
  EXPECT_EQ(result.games[0].installation, result.games[1].installation);
  EXPECT_EQ(result.launchers[6].toMap().value("count").toInt(), 1);
}

TEST_F(LauncherLibraryTest, ItchReceiptRejectsLinkedMetadataDirectoryAndFifoReceipt) {
  itch({});
  pe(exe);
  receipt({"bin/Game.exe"});
  const auto metadata = directory + "/.itch";
  ASSERT_TRUE(QDir().rename(metadata, home + "/foreign-metadata"));
  ASSERT_TRUE(QFile::link(home + "/foreign-metadata", metadata));
  EXPECT_TRUE(read().isEmpty());
  ASSERT_TRUE(QFile::remove(metadata)); // Remove only the fixture symlink.
  ASSERT_TRUE(QDir().mkpath(metadata));
  ASSERT_EQ(::mkfifo(QFile::encodeName(metadata + "/receipt.json.gz").constData(), 0600), 0);
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, ItchReceiptBoundsCompressedInputAndFileList) {
  itch({});
  pe(exe);
  write(directory + "/.itch/receipt.json.gz", QByteArray(4 * 1024 * 1024 + 1, '!'));
  EXPECT_TRUE(read().isEmpty());
  QJsonArray files;
  for (int i = 0; i <= 32768; ++i)
    files.append("bin/Game.exe");
  receipt(files);
  EXPECT_TRUE(read().isEmpty());
  receipt({"bin/Game.exe"});
  ASSERT_EQ(read().size(), 1);
}

TEST_F(LauncherLibraryTest, EveryLauncherDetectsConfigurationWithoutInstalledGames) {
  const QStringList roots{data + "/Steam", config + "/heroic", config + "/legendary",
      config + "/nile", config + "/lutris", config + "/faugus-launcher", config + "/itch"};
  const QStringList files{"config/config.vdf", "config.json", "config.ini",
      "current_user.json", "lutris.conf", "config.json", "config.json"};
  const QList<QByteArray> contents{
      "\"InstallConfigStore\" { \"Software\" { \"Valve\" {} } }\n",
      "{}", "[Legendary]\ninstall_dir = ~/Games\n", "{}", "[lutris]\n", "{}", "{}"};
  for (int i = 0; i < roots.size(); ++i) {
    SCOPED_TRACE(i);
    write(roots[i] + "/" + files[i], contents[i]);
  }
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.games.isEmpty());
  for (int i = 0; i < roots.size(); ++i) {
    SCOPED_TRACE(i);
    const auto launcher = result.launchers[i].toMap();
    EXPECT_TRUE(launcher.value("detected").toBool());
    EXPECT_EQ(launcher.value("count").toInt(), 0);
    EXPECT_TRUE(launcher.value("games").toList().isEmpty());
    const auto locations = launcher.value("locations").toList();
    ASSERT_FALSE(locations.isEmpty());
    EXPECT_EQ(locations.first().toMap().value("status").toString(), "Detected");
    EXPECT_TRUE(result.watchPaths.contains(roots[i] + "/" + files[i]));
  }
}

TEST_F(LauncherLibraryTest, ConfigurationOnlyDetectionRespectsCustomAndDisabledLocations) {
  const QStringList ids{"steam", "heroic", "legendary", "nile", "lutris", "faugus", "itch"};
  const QStringList files{"config/config.vdf", "config.json", "config.ini",
      "library.json", "lutris.conf", "config.json", "preferences.json"};
  const QList<QByteArray> contents{"\"InstallConfigStore\" {}", "{}",
      "[Legendary]\n", "[]", "[lutris]\n", "{}", "{}"};
  QVariantMap preferences;
  for (int i = 0; i < ids.size(); ++i) {
    const auto root = home + "/custom-" + ids[i];
    write(root + "/" + files[i], contents[i]);
    preferences[ids[i]] = QVariantMap{{"automatic", false}, {"paths", QStringList{root}}};
  }
  auto result = LauncherLibrary::discover(home, config, data, preferences);
  EXPECT_TRUE(result.games.isEmpty());
  for (const auto &row : result.launchers) {
    EXPECT_TRUE(row.toMap().value("detected").toBool());
    EXPECT_EQ(row.toMap().value("count").toInt(), 0);
  }
  for (const auto &id : ids)
    preferences[id] = QVariantMap{{"automatic", false}};
  result = LauncherLibrary::discover(home, config, data, preferences);
  for (const auto &row : result.launchers)
    EXPECT_FALSE(row.toMap().value("detected").toBool());
}

TEST_F(LauncherLibraryTest, ConfigurationOnlyDetectionSupportsFlatpakRoots) {
  object(home + "/.var/app/com.heroicgameslauncher.hgl/config/heroic/config.json", {});
  object(home + "/.var/app/io.itch.itch/config/itch/preferences.json", {});
  write(home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/config/config.vdf",
        "\"InstallConfigStore\" {}");
  write(home + "/.var/app/net.lutris.Lutris/config/lutris/lutris.conf", "[lutris]\n");
  const auto result = LauncherLibrary::discover(home, config, data);
  for (const auto i : {0, 1, 4, 6}) {
    SCOPED_TRACE(i);
    EXPECT_TRUE(result.launchers[i].toMap().value("detected").toBool());
    EXPECT_EQ(result.launchers[i].toMap().value("count").toInt(), 0);
  }
}

TEST_F(LauncherLibraryTest, ConfigurationDoesNotHideInvalidLibraries) {
  object(config + "/heroic/config.json", {});
  write(config + "/heroic/gog_store/installed.json", "{broken");
  write(config + "/legendary/config.ini", "[Legendary]\n");
  write(config + "/legendary/installed.json", "{broken");
  object(config + "/nile/current_user.json", {});
  write(config + "/nile/installed.json", "{broken");
  write(config + "/lutris/lutris.conf", "[lutris]\n");
  write(data + "/lutris/pga.db", "not a database");
  object(config + "/itch/config.json", {});
  write(config + "/itch/db/butler.db", "not a database");
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.games.isEmpty());
  for (const auto i : {1, 2, 3, 4, 6}) {
    SCOPED_TRACE(i);
    const auto row = result.launchers[i].toMap();
    EXPECT_TRUE(row.value("detected").toBool());
    ASSERT_FALSE(row.value("locations").toList().isEmpty());
    EXPECT_EQ(row.value("locations").toList().first().toMap().value("status").toString(),
              "Unreadable or invalid metadata");
  }
}

TEST_F(LauncherLibraryTest, EmptyDirectoriesAndInvalidConfigurationDoNotDetectLaunchers) {
  const QStringList roots{data + "/Steam", config + "/heroic", config + "/legendary",
      config + "/nile", config + "/lutris", config + "/faugus-launcher", config + "/itch"};
  const QStringList files{"config/config.vdf", "config.json", "config.ini",
      "current_user.json", "lutris.conf", "config.json", "config.json"};
  for (const auto &root : roots)
    ASSERT_TRUE(QDir().mkpath(root));
  auto result = LauncherLibrary::discover(home, config, data);
  for (const auto &row : result.launchers)
    EXPECT_FALSE(row.toMap().value("detected").toBool());
  for (const auto &bad : QList<QByteArray>{"garbage", "[]", QByteArray(4 * 1024 * 1024 + 1, 'x')}) {
    for (int i = 0; i < roots.size(); ++i)
      write(roots[i] + "/" + files[i], bad);
    result = LauncherLibrary::discover(home, config, data);
    EXPECT_TRUE(result.games.isEmpty());
    for (const auto &row : result.launchers)
      EXPECT_FALSE(row.toMap().value("detected").toBool());
  }
}

TEST_F(LauncherLibraryTest, SteamConfigurationRejectsTruncatedOrUnrelatedVdf) {
  for (const auto &bad : QList<QByteArray>{"\"InstallConfigStore\" {", "\"Other\" {}",
          "\"InstallConfigStore\" { \"key\" }", "\"InstallConfigStore\" {} trailing",
          "\"InstallConfigStore\" {} }"}) {
    write(data + "/Steam/config/config.vdf", bad);
    EXPECT_FALSE(LauncherLibrary::discover(home, config, data).launchers[0].toMap()
                     .value("detected").toBool());
  }
}

TEST_F(LauncherLibraryTest, FaugusReadsExactWinePayloadAndConfiguredPrefix) {
  const auto prefix = home + "/Faugus/a-game";
  array(data + "/faugus-launcher/games.json",
        {QJsonObject{{"gameid", "a-game"}, {"title", "A Faugus Game"},
                     {"path", exe}, {"prefix", prefix}, {"runner", "Proton-GE Latest"}}});
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games[0].sources, QStringList{"Faugus"});
  EXPECT_EQ(result.games[0].executable, exe);
  EXPECT_EQ(result.games[0].winePrefix, prefix);
  EXPECT_TRUE(result.games[0].directory.isEmpty());
  EXPECT_TRUE(result.launchers[5].toMap().value("detected").toBool());
  EXPECT_TRUE(result.watchPaths.contains(data + "/faugus-launcher/games.json"));
  auto p = process();
  p.winePrefix = prefix;
  EXPECT_EQ(identify({{p.pid, p}}, result.games).size(), 1);
  p.winePrefix = home + "/Other Prefix";
  EXPECT_TRUE(identify({{p.pid, p}}, result.games).isEmpty());
  p.winePrefix = prefix;
  p.executable = directory + "/Unrelated.exe";
  p.arguments = {p.executable};
  EXPECT_TRUE(identify({{p.pid, p}}, result.games).isEmpty());
}

TEST_F(LauncherLibraryTest, FaugusConfigurationDetectsFreshSetupWithZeroGames) {
  object(config + "/faugus-launcher/config.json", {{"interface-mode", "grid"}});
  write(data + "/faugus-launcher/version.txt", "runtime installed");
  const auto result = LauncherLibrary::discover(home, config, data);
  const auto launcher = result.launchers[5].toMap();
  EXPECT_TRUE(launcher.value("detected").toBool());
  EXPECT_EQ(launcher.value("count").toInt(), 0);
  EXPECT_TRUE(launcher.value("games").toList().isEmpty());
  EXPECT_TRUE(result.games.isEmpty());
  const auto locations = launcher.value("locations").toList();
  ASSERT_EQ(locations.size(), 1);
  EXPECT_EQ(locations[0].toMap().value("path").toString(), config + "/faugus-launcher");
  EXPECT_EQ(locations[0].toMap().value("status").toString(), "Detected");
  EXPECT_EQ(locations[0].toMap().value("count").toInt(), 0);
  EXPECT_TRUE(result.watchPaths.contains(config + "/faugus-launcher/config.json"));
  EXPECT_TRUE(result.watchPaths.contains(data + "/faugus-launcher"));
  EXPECT_FALSE(QFileInfo::exists(data + "/faugus-launcher/games.json"));
}

TEST_F(LauncherLibraryTest, FaugusConfigurationDetectionSupportsFlatpakAndCustomLocations) {
  const auto flatpak = home + "/.var/app/io.github.Faugus.faugus-launcher/config/faugus-launcher";
  object(flatpak + "/config.json", {});
  object(home + "/custom-faugus/config.json", {});
  auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.launchers[5].toMap().value("detected").toBool());
  EXPECT_EQ(result.launchers[5].toMap().value("count").toInt(), 0);
  result = LauncherLibrary::discover(home, config, data,
      {{"faugus", QVariantMap{{"automatic", false},
          {"paths", QStringList{"~/custom-faugus"}}}}});
  const auto launcher = result.launchers[5].toMap();
  EXPECT_TRUE(launcher.value("detected").toBool());
  EXPECT_EQ(launcher.value("count").toInt(), 0);
  ASSERT_EQ(launcher.value("locations").toList().size(), 1);
  EXPECT_EQ(launcher.value("locations").toList()[0].toMap().value("path").toString(),
            home + "/custom-faugus");
}

TEST_F(LauncherLibraryTest, FaugusConfigurationDoesNotHideInvalidGameLibrary) {
  const auto root = config + "/faugus-launcher";
  object(root + "/config.json", {});
  write(root + "/games.json", "{bad json");
  const auto result = LauncherLibrary::discover(home, config, data);
  const auto launcher = result.launchers[5].toMap();
  EXPECT_TRUE(launcher.value("detected").toBool());
  EXPECT_TRUE(result.games.isEmpty());
  ASSERT_EQ(launcher.value("locations").toList().size(), 1);
  EXPECT_EQ(launcher.value("locations").toList()[0].toMap().value("status").toString(),
            "Unreadable or invalid metadata");
}

TEST_F(LauncherLibraryTest, FaugusConfigurationDoesNotInventGameEntriesOrAcceptWrongShape) {
  const auto root = config + "/faugus-launcher";
  object(root + "/config.json", {{"title", "Not a game"}, {"path", exe},
                                {"prefix", home + "/prefix"}, {"runner", "Default"}});
  auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.launchers[5].toMap().value("detected").toBool());
  EXPECT_TRUE(result.games.isEmpty());
  array(root + "/config.json", {});
  result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.launchers[5].toMap().value("locations").toList().size(), 1);
  EXPECT_EQ(result.launchers[5].toMap().value("locations").toList()[0].toMap()
                .value("status").toString(), "Unreadable or invalid metadata");
}

TEST_F(LauncherLibraryTest, FaugusFreshSetupTracksGameLibraryCreationAndRemoval) {
  object(config + "/faugus-launcher/config.json", {});
  EXPECT_TRUE(LauncherLibrary::discover(home, config, data).launchers[5].toMap()
                  .value("detected").toBool());
  array(data + "/faugus-launcher/games.json", {QJsonObject{
      {"title", "New Faugus Game"}, {"path", exe}, {"prefix", home + "/prefix"},
      {"runner", "Default"}}});
  auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.launchers[5].toMap().value("detected").toBool());
  EXPECT_EQ(result.launchers[5].toMap().value("count").toInt(), 1);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games.first().name, "New Faugus Game");
  ASSERT_TRUE(QFile::remove(data + "/faugus-launcher/games.json"));
  result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.launchers[5].toMap().value("detected").toBool());
  EXPECT_EQ(result.launchers[5].toMap().value("count").toInt(), 0);
  EXPECT_TRUE(result.games.isEmpty());
}

TEST_F(LauncherLibraryTest, FaugusNativeAndFlatpakAndLegacyLocationsAreCombined) {
  write(directory + "/NativeGame", QByteArray("\x7f""ELFfixture", 11));
  const auto flatpak = home + "/.var/app/io.github.Faugus.faugus-launcher";
  const QJsonObject entry{{"title", "Native Faugus Game"},
      {"path", directory + "/NativeGame"}, {"runner", "Linux-Native"}};
  for (const auto &path : QStringList{data + "/faugus-launcher",
       config + "/faugus-launcher", flatpak + "/data/faugus-launcher",
       flatpak + "/config/faugus-launcher"})
    array(path + "/games.json", {entry});
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_TRUE(result.games[0].winePrefix.isEmpty());
  EXPECT_FALSE(result.games[0].bootstrap);
  EXPECT_EQ(result.launchers[5].toMap().value("locations").toList().size(), 4);
  EXPECT_EQ(result.launchers[5].toMap().value("count").toInt(), 1);
}

TEST_F(LauncherLibraryTest, FaugusCustomLocationsRespectAutomaticToggleAndHomeExpansion) {
  const QJsonObject entry{{"title", "Custom Faugus Game"}, {"path", "~/Games/A Game/bin/Game.exe"},
      {"prefix", "~/Faugus/a-game"}, {"runner", "Default"}};
  array(home + "/custom-faugus/games.json", {entry});
  array(data + "/faugus-launcher/games.json", {QJsonObject{
      {"title", "Automatic Game"}, {"path", exe}, {"prefix", home + "/prefix"}}});
  const auto result = LauncherLibrary::discover(home, config, data,
      {{"faugus", QVariantMap{{"automatic", false},
                              {"paths", QStringList{home + "/custom-faugus"}}}}});
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games[0].name, "Custom Faugus Game");
  EXPECT_EQ(result.games[0].executable, exe);
  EXPECT_EQ(result.games[0].winePrefix, home + "/Faugus/a-game");
}

TEST_F(LauncherLibraryTest, FaugusSkipsSteamScriptsCommandsMissingPrefixesAndAdditionalAppModes) {
  QJsonArray entries;
  const QJsonObject valid{{"title", "A Game"}, {"path", exe}, {"prefix", home + "/prefix"},
                          {"runner", "Proton-GE Latest"}};
  for (const auto &change : QList<QPair<QString, QJsonValue>>{
       {"runner", "Steam"}, {"runner", QJsonObject{}}, {"prefix", ""},
       {"path", "bin/Game.exe"}, {"path", "C:\\Game.exe"}, {"path", "$HOME/Game.exe"},
       {"path", "/missing/Game.exe"}, {"title", ""}, {"addapp_enabled", true},
       {"addapp_enabled", "addapp_enabled"}, {"addapp_enabled", QJsonObject{}},
       {"runner", "Linux-Native"}}) {
    auto entry = valid;
    entry[change.first] = change.second;
    entries.append(entry);
  }
  write(directory + "/start.sh", "#!/bin/sh\n");
  auto script = valid;
  script["path"] = directory + "/start.sh";
  entries.append(script);
  array(data + "/faugus-launcher/games.json", entries);
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, FaugusDoesNotAttestGenericBootstrapsOrSharedStorefronts) {
  write(directory + "/Launcher.exe", "MZfixture");
  write(directory + "/Amazon Games.exe", "MZfixture");
  write(directory + "/wgc.exe", "MZfixture");
  QJsonArray entries;
  for (const auto &path : QStringList{"Launcher.exe", "Amazon Games.exe", "wgc.exe"})
    entries.append(QJsonObject{{"title", "A Game"}, {"path", directory + "/" + path},
                               {"prefix", home + "/prefix"}});
  entries.append(QJsonObject{{"gameid", "epic"}, {"title", "Storefront"},
                            {"path", exe}, {"prefix", home + "/prefix"}});
  array(data + "/faugus-launcher/games.json", entries);
  EXPECT_TRUE(read().isEmpty());
}

TEST_F(LauncherLibraryTest, FaugusMalformedWrongShapeAndOversizedMetadataIsRejected) {
  const auto file = data + "/faugus-launcher/games.json";
  for (const auto &contents : QList<QByteArray>{"{malformed", "{\"games\":[]}",
                                               QByteArray(4 * 1024 * 1024 + 1, ' ')}) {
    write(file, contents);
    const auto result = LauncherLibrary::discover(home, config, data);
    EXPECT_TRUE(result.games.isEmpty());
    const auto locations = result.launchers[5].toMap().value("locations").toList();
    ASSERT_EQ(locations.size(), 1);
    EXPECT_EQ(locations[0].toMap().value("status").toString(), "Unreadable or invalid metadata");
  }
}

TEST_F(LauncherLibraryTest, FaugusConflictingNamesNeedUserRulesAndKeepConcurrentInstances) {
  const auto prefix = home + "/prefix";
  array(data + "/faugus-launcher/games.json", {
      QJsonObject{{"title", "First Name"}, {"path", exe}, {"prefix", prefix}},
      QJsonObject{{"title", "Second Name"}, {"path", exe}, {"prefix", prefix}}});
  auto a = process(71), b = process(72);
  a.winePrefix = b.winePrefix = prefix;
  const auto catalogue = read();
  EXPECT_TRUE(identify({{71, a}, {72, b}}, catalogue).isEmpty());
  const QMap<QString, QString> rules{{
      GameWhitelist::normalizeKey("wine:" + prefix + ":" + exe), "User Name"}};
  EXPECT_EQ(identify({{71, a}, {72, b}}, catalogue, rules).size(), 2);
}

TEST_F(LauncherLibraryTest, MissingMetadataWatchesNearestParentThenNewDirectoriesAndFiles) {
  const auto path = home + "/missing/nested/legendary";
  const QVariantMap preferences{{"legendary", QVariantMap{
      {"automatic", false}, {"paths", QStringList{path}}}}};
  auto result = LauncherLibrary::discover(home, config, data, preferences);
  EXPECT_TRUE(result.watchPaths.contains(home));
  ASSERT_TRUE(QDir().mkpath(path));
  result = LauncherLibrary::discover(home, config, data, preferences);
  EXPECT_TRUE(result.watchPaths.contains(path));
  object(path + "/installed.json", {{"game", epic()}});
  result = LauncherLibrary::discover(home, config, data, preferences);
  EXPECT_TRUE(result.watchPaths.contains(path + "/installed.json"));
  EXPECT_TRUE(result.watchPaths.contains(path));
}

TEST_F(LauncherLibraryTest, WatchPathsIncludeSteamExtraLibraryAndManifests) {
  const auto steam = home + "/.steam/steam";
  const auto extra = home + "/Extra Drive/steamapps";
  write(steam + "/steamapps/libraryfolders.vdf",
        ("\"libraryfolders\" { \"1\" { \"path\" \"" + home + "/Extra Drive\" } }").toUtf8());
  write(extra + "/appmanifest_123.acf",
        "\"AppState\" { \"appid\" \"123\" \"name\" \"Game\" \"installdir\" \"Game\" }");
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.watchPaths.contains(steam + "/steamapps/libraryfolders.vdf"));
  EXPECT_TRUE(result.watchPaths.contains(extra));
  EXPECT_TRUE(result.watchPaths.contains(extra + "/appmanifest_123.acf"));
}

TEST_F(LauncherLibraryTest, WatchPathsIncludeLutrisWalAndSeparateYamlFiles) {
  lutris("game:\n  exe: bin/Game.exe\n", "linux");
  const auto database = data + "/lutris/pga.db";
  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open(QFile::encodeName(database).constData(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "PRAGMA journal_mode=WAL; UPDATE games SET name='WAL Game';",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.watchPaths.contains(database));
  EXPECT_TRUE(result.watchPaths.contains(database + "-wal"));
  EXPECT_TRUE(result.watchPaths.contains(config + "/lutris/games"));
  EXPECT_TRUE(result.watchPaths.contains(config + "/lutris/games/a-game.yml"));
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games[0].name, "WAL Game");
  sqlite3_close(db);
}

TEST_F(LauncherLibraryTest, WatchPathsIncludeGogInfoAndAmazonLibraryAndFuel) {
  object(config + "/heroic/gog_store/installed.json",
         {{"installed", QJsonArray{QJsonObject{{"appName", "123"}, {"install_path", directory}}}}});
  object(directory + "/goggame-123.info", {{"gameId", "123"}});
  const auto path = config + "/heroic/nile_config/nile";
  array(path + "/installed.json", {QJsonObject{{"id", "amazon-id"}, {"path", directory}}});
  array(path + "/library.json", {});
  object(directory + "/fuel.json", {});
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.watchPaths.contains(directory + "/goggame-123.info"));
  EXPECT_TRUE(result.watchPaths.contains(directory + "/fuel.json"));
  EXPECT_TRUE(result.watchPaths.contains(path + "/library.json"));
  EXPECT_TRUE(result.watchPaths.contains(path + "/installed.json"));
}

TEST_F(LauncherLibraryTest, FindsFlatpakHeroicAndDeduplicatesCustomSymlink) {
  const auto path = home + "/.var/app/com.heroicgameslauncher.hgl/config/heroic";
  object(path + "/legendaryConfig/legendary/installed.json", {{"game", epic()}});
  const auto alias = home + "/heroic-alias";
  ASSERT_TRUE(QFile::link(path, alias));
  const QVariantMap preferences{{"heroic", QVariantMap{{"paths", QStringList{alias}}}}};
  const auto result = LauncherLibrary::discover(home, config, data, preferences);
  ASSERT_EQ(result.games.size(), 1);
  const auto heroic = result.launchers[1].toMap();
  EXPECT_TRUE(heroic.value("detected").toBool());
  EXPECT_EQ(heroic.value("locations").toList().size(), 1);
  ASSERT_EQ(heroic.value("games").toList().size(), 1);
  EXPECT_EQ(heroic.value("games").toList()[0].toMap().value("name").toString(), "An Epic Game");
}

TEST_F(LauncherLibraryTest, CustomLocationsCanReplaceAutomaticAndReportMissing) {
  object(config + "/legendary/installed.json", {{"native", epic()}});
  const auto custom = home + "/custom metadata";
  auto entry = epic();
  entry["title"] = "Custom Game";
  object(custom + "/installed.json", {{"custom", entry}});
  const QVariantMap preferences{{"legendary", QVariantMap{
      {"automatic", false}, {"paths", QStringList{custom, home + "/missing"}}}}};
  const auto result = LauncherLibrary::discover(home, config, data, preferences);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games[0].name, "Custom Game");
  const auto launcher = result.launchers[2].toMap();
  EXPECT_FALSE(launcher.value("automatic").toBool());
  const auto locations = launcher.value("locations").toList();
  ASSERT_EQ(locations.size(), 2);
  EXPECT_EQ(locations[1].toMap().value("status").toString(), "Not found");
}

TEST_F(LauncherLibraryTest, ReportsInvalidMetadataRatherThanAnEmptyLibrary) {
  write(config + "/legendary/installed.json", "{invalid");
  const auto result = LauncherLibrary::discover(home, config, data);
  const auto launcher = result.launchers[2].toMap();
  EXPECT_TRUE(launcher.value("detected").toBool());
  EXPECT_EQ(launcher.value("locations").toList()[0].toMap().value("status").toString(),
            "Unreadable or invalid metadata");
  EXPECT_TRUE(result.games.isEmpty());
}

TEST_F(LauncherLibraryTest, LegendaryEnvironmentLocationRespectsAutomaticToggle) {
  const auto alternate = home + "/legendary-config";
  object(alternate + "/installed.json", {{"game", epic()}});
  auto result = LauncherLibrary::discover(home, config, data, {}, alternate);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_TRUE(result.launchers[2].toMap().value("detected").toBool());
  result = LauncherLibrary::discover(home, config, data,
      {{"legendary", QVariantMap{{"automatic", false}}}}, alternate);
  EXPECT_TRUE(result.games.isEmpty());
  EXPECT_FALSE(result.launchers[2].toMap().value("detected").toBool());
}

TEST_F(LauncherLibraryTest, EmptyMetadataIsDetectedWithoutInventingGames) {
  object(config + "/legendary/installed.json", {});
  const auto result = LauncherLibrary::discover(home, config, data);
  EXPECT_TRUE(result.games.isEmpty());
  EXPECT_TRUE(result.launchers[2].toMap().value("detected").toBool());
  EXPECT_EQ(result.launchers[2].toMap().value("count").toInt(), 0);
}

TEST_F(LauncherLibraryTest, SteamSharedSecondaryLibraryIsOnlyCountedOnce) {
  const auto first = home + "/.steam/steam";
  const auto second = home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam";
  const auto library = home + "/other drive";
  for (const auto &path : {first, second})
    write(path + "/steamapps/libraryfolders.vdf",
          ("\"libraryfolders\" { \"1\" { \"path\" \"" + library + "\" } }").toUtf8());
  write(library + "/steamapps/appmanifest_123.acf",
        "\"AppState\" { \"appid\" \"123\" \"name\" \"A Steam Game\" \"installdir\" \"Game\" }");
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.launchers[0].toMap().value("count").toInt(), 1);
  EXPECT_EQ(result.launchers[0].toMap().value("locations").toList().size(), 2);
}

TEST_F(LauncherLibraryTest, FindsFlatpakLutrisWithSeparateConfigAndData) {
  lutris("game:\n  exe: " + exe + "\n  prefix: " + home + "/prefix\n");
  const auto base = home + "/.var/app/net.lutris.Lutris";
  ASSERT_TRUE(QDir().mkpath(base + "/data/lutris"));
  ASSERT_TRUE(QDir().mkpath(base + "/config/lutris/games"));
  ASSERT_TRUE(QFile::rename(data + "/lutris/pga.db", base + "/data/lutris/pga.db"));
  ASSERT_TRUE(QFile::rename(config + "/lutris/games/a-game.yml",
                           base + "/config/lutris/games/a-game.yml"));
  const auto result = LauncherLibrary::discover(home, config, data);
  ASSERT_EQ(result.games.size(), 1);
  EXPECT_EQ(result.games[0].name, "A Lutris Game");
  EXPECT_TRUE(result.launchers[4].toMap().value("detected").toBool());
}

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

TEST_F(LauncherLibraryTest, SteamSupportToolsUseExactIdsAndManifestRolesNotTitles) {
  const auto apps = data + "/Steam/steamapps";
  const QList<uint> ids{228980, 1070560, 1161040, 1391110, 1493710, 1628350,
                        1826330, 2180100, 2805730, 4183110, 4628710};
  for (uint id : ids) {
    write(apps + QString("/appmanifest_%1.acf").arg(id),
          QString("\"appid\" \"%1\" \"name\" \"Support Tool %1\" \"installdir\" \"tool-%1\"")
              .arg(id).toUtf8());
  }
  for (const auto &layer : QStringList{"proton", "scout-in-container"}) {
    const uint id = layer == "proton" ? 4005 : 4006;
    write(apps + QString("/appmanifest_%1.acf").arg(id),
          QString("\"appid\" \"%1\" \"name\" \"Future Tool\" \"installdir\" \"future-%1\"")
              .arg(id).toUtf8());
    write(apps + QString("/common/future-%1/toolmanifest.vdf").arg(id),
          QString("\"manifest\" { \"compatmanager_layer_name\" \"%1\" }").arg(layer).toUtf8());
  }
  write(apps + "/appmanifest_42.acf",
        "\"appid\" \"42\" \"name\" \"Proton Runtime Adventures\" \"installdir\" \"RealGame\"");
  const auto games = read();
  ASSERT_EQ(games.size(), ids.size() + 3);
  for (const auto &game : games)
    EXPECT_EQ(game.runtimeTool, game.steamId != 42) << game.steamId;
  const auto discovery = LauncherLibrary::discover(home, config, data);
  ASSERT_FALSE(discovery.launchers.isEmpty());
  EXPECT_EQ(discovery.launchers.first().toMap().value("count").toInt(), 1);
}

TEST_F(LauncherLibraryTest, SteamRuntimeManifestExcludesItsToolsWithoutExcludingHostedGames) {
  const auto apps = data + "/Steam/steamapps";
  const auto runtime = apps + "/common/SteamLinuxRuntime_4";
  write(apps + "/appmanifest_4004.acf",
        "\"appid\" \"4004\" \"name\" \"Steam Linux Runtime 4.0\" "
        "\"installdir\" \"SteamLinuxRuntime_4\"");
  write(runtime + "/toolmanifest.vdf",
        "\"manifest\" { \"compatmanager_layer_name\" \"container-runtime\" }");
  write(apps + "/appmanifest_42.acf",
        "\"appid\" \"42\" \"name\" \"Actual Game\" \"installdir\" \"Game\"");
  const auto games = read();
  ASSERT_EQ(games.size(), 2);
  const auto tool = std::find_if(games.begin(), games.end(),
                               [](const auto &g) { return g.steamId == 4004; });
  ASSERT_NE(tool, games.end());
  EXPECT_TRUE(tool->runtimeTool);
  auto helper = process();
  auto gameProcess = process(71);
  gameProcess.executable = apps + "/common/Game/game";
  gameProcess.arguments = {gameProcess.executable};
  gameProcess.parent = helper.pid;
  gameProcess.steamId = 42;
  for (const auto &relative : QStringList{
           "/pressure-vessel/bin/pv-verify",
           "/pressure-vessel/libexec/steam-runtime-tools-0/x86_64-linux-gnu-check-vulkan",
           "/pressure-vessel/libexec/steam-runtime-tools-0/i386-linux-gnu-inspect-library",
           "/_v2-entry-point", "/run", "/new-helper-not-in-name-list"}) {
    SCOPED_TRACE(relative.toStdString());
    helper.executable = runtime + relative;
    helper.arguments = {helper.executable};
    helper.steamId = 4004;
    EXPECT_TRUE(identify({{helper.pid, helper}}, games).isEmpty());
    // Inheriting the game's App ID and a saved rule cannot promote a tool.
    helper.steamId = 42;
    const QMap<QString, QString> rules{{"steam_app_42", "Actual Game"}};
    EXPECT_TRUE(identify({{helper.pid, helper}}, games, rules).isEmpty());
    const auto result = identify({{helper.pid, helper}, {gameProcess.pid, gameProcess}}, games, rules);
    ASSERT_EQ(result.size(), 1);
    EXPECT_EQ(result.first().primaryPid, gameProcess.pid);
    EXPECT_EQ(result.first().processes.size(), 1);
  }
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
