/*
 * Trophy - Unit Tests
 * Test suite for GameWhitelist CRUD and persistence
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "detection/GameWhitelist.h"
#include "detection/DefaultAppCatalog.h"

using namespace TrophyBackend;

class GameWhitelistTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
    // Use test-specific config path
    QCoreApplication::setOrganizationName("TrophyTest");
    QCoreApplication::setApplicationName("TrophyTestWhitelist");
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new GameWhitelistTestEnvironment);

class GameWhitelistTest : public ::testing::Test {
protected:
  // NOTE: declared before wl so it outlives it.
  QTemporaryDir configHome;
  QByteArray oldXdgConfigHome;
  std::unique_ptr<GameWhitelist> wl;

  void SetUp() override {
    ASSERT_TRUE(configHome.isValid());
    // Isolate from the user's real ~/.config/trophy config: GameWhitelist
    // resolves its path via XDG_CONFIG_HOME on Linux.
    oldXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    wl = std::make_unique<GameWhitelist>();
  }

  void TearDown() override {
    wl.reset();
    if (oldXdgConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldXdgConfigHome);
  }
};

// --- Built-in default state ---

TEST_F(GameWhitelistTest, WhitelistEmptyByDefault) {
  EXPECT_TRUE(wl->whitelistEntries().isEmpty());
}

TEST_F(GameWhitelistTest, KeysNormalizeCaseAndExeSuffix) {
  wl->addToWhitelist("DeadLock.EXE", "Deadlock");
  EXPECT_TRUE(wl->isWhitelisted("deadlock"));
  EXPECT_TRUE(wl->isWhitelisted("Deadlock.exe"));
  EXPECT_TRUE(wl->isWhitelisted("DEADLOCK"));
  EXPECT_TRUE(wl->whitelistEntries().contains("deadlock"));
  EXPECT_EQ(wl->getWhitelistName("DeadLock.exe"), "Deadlock");

  wl->addToBlacklist("OBS64");
  EXPECT_TRUE(wl->isBlacklisted("obs64"));
  wl->removeFromBlacklist("Obs64.EXE");
  EXPECT_FALSE(wl->isBlacklisted("OBS64"));

  // Blank keys are refused without touching the lists.
  const int before = wl->whitelistEntries().size();
  wl->addToWhitelist("  ");
  wl->addToBlacklist("");
  EXPECT_EQ(wl->whitelistEntries().size(), before);
}

TEST_F(GameWhitelistTest, DefaultListCollapsedToCanonical) {
  // One row per app: no "Steam"/"steam" or "xalia"/"xalia.exe" dupes.
  const QStringList entries = wl->blacklistEntries();
  EXPECT_FALSE(entries.contains("Steam"));
  EXPECT_FALSE(entries.contains("Discord"));
  EXPECT_FALSE(entries.contains("xalia.exe"));
  EXPECT_TRUE(entries.contains("steam"));
  // ...but every spelling still matches.
  EXPECT_TRUE(wl->isBlacklisted("Steam"));
  EXPECT_TRUE(wl->isBlacklisted("steam.exe"));
  EXPECT_TRUE(wl->isBlacklisted("DISCORD"));
  EXPECT_TRUE(wl->isBlacklisted("Xalia.EXE"));
}

TEST_F(GameWhitelistTest, PrerequisiteInstallersAreDefaultExclusions) {
  const QStringList installers{
      "VC_REDIST.X86.EXE", "vc_redist.x64.exe", "vc_redist.arm64.exe",
      "vcredist_x86.exe", "vcredist_x64.exe", "vcredist_arm64.exe",
      "vcredist_ia64.exe", "DXSETUP.EXE"};
  wl->save();
  wl->load();
  for (const QString &installer : installers) {
    SCOPED_TRACE(installer.toStdString());
    EXPECT_TRUE(wl->isBlacklisted(installer));
    EXPECT_TRUE(GameWhitelist::defaultBlacklistEntries().contains(
        defaultExclusionKey(GameWhitelist::normalizeKey(installer))));
  }
  EXPECT_FALSE(wl->isBlacklisted("steam_proton"));
  EXPECT_FALSE(wl->isBlacklisted("setup.exe"));
  EXPECT_FALSE(wl->isBlacklisted("vc_redist.x64-adventure.exe"));
}

TEST_F(GameWhitelistTest, LegacyRedistributableRowsConsolidateOnLoad) {
  const QString path = configHome.path() + "/trophy/game-whitelist.json";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(R"({"blacklist":["vc_redist.x86","vc_redist.x64","vcredist_x86","dxsetup"]})");
  file.close();
  wl->load();
  EXPECT_EQ(wl->blacklistEntries().count("vc_redist"), 1);
  EXPECT_TRUE(wl->blacklistEntries().contains("dxsetup"));
  for (const QString &alias : visualCppInstallerNames()) {
    EXPECT_FALSE(wl->blacklistEntries().contains(alias));
    EXPECT_TRUE(wl->isBlacklisted(alias));
  }
  EXPECT_EQ(GameWhitelist::prettySystemName("vc_redist"), "vc_redist");
}

TEST_F(GameWhitelistTest, DefaultAliasesMatchExactlyAndStayOnOneVetoRow) {
  const auto defaults = GameWhitelist::defaultBlacklistEntries();
  for (const auto &app : defaultAppCatalog()) {
    SCOPED_TRACE(app.key.toStdString());
    EXPECT_TRUE(defaults.contains(app.key));
    EXPECT_TRUE(wl->isBlacklisted(app.key));
    for (const QString &alias : app.aliases) {
      EXPECT_TRUE(wl->isBlacklisted(alias));
      EXPECT_TRUE(wl->isBlacklisted(alias.toUpper() + ".EXE"));
      EXPECT_FALSE(wl->isBlacklisted(alias + ".game"));
      EXPECT_FALSE(wl->isBlacklisted("game-" + alias));
      EXPECT_EQ(GameWhitelist::normalizeKey(alias), alias);
    }
  }
  EXPECT_EQ(defaultAppCatalog().size(), defaults.size());
  EXPECT_TRUE(wl->isBlacklisted("com.obsproject.Studio"));
  EXPECT_FALSE(wl->isBlacklisted("exe:/games/com.obsproject.studio"));
  EXPECT_FALSE(wl->isBlacklisted("my-obs-game"));
  // Presentation-only aliases must not become detection rules.
  EXPECT_FALSE(wl->isBlacklisted("utilities-terminal"));
  EXPECT_FALSE(wl->isBlacklisted("system-file-manager"));
  wl->addToBlacklist("com.obsproject.Studio");
  EXPECT_FALSE(wl->blacklistEntries().contains("com.obsproject.studio"));
  EXPECT_EQ(wl->blacklistEntries().count("obs"), 1);
}

TEST_F(GameWhitelistTest, DefaultAliasUnblockReblockAndPersistence) {
  for (const auto &app : defaultAppCatalog()) {
    if (app.aliases.isEmpty())
      continue;
    SCOPED_TRACE(app.key.toStdString());
    wl->removeFromBlacklist(app.aliases.first());
    EXPECT_FALSE(wl->isBlacklisted(app.key));
    for (const QString &alias : app.aliases)
      EXPECT_FALSE(wl->isBlacklisted(alias));
  }
  wl->save();
  GameWhitelist restored;
  restored.load();
  EXPECT_FALSE(restored.isBlacklisted("obs"));
  EXPECT_FALSE(restored.isBlacklisted("com.obsproject.Studio"));
  restored.addToBlacklist("OBS-STUDIO.EXE");
  EXPECT_TRUE(restored.isBlacklisted("obs"));
  EXPECT_TRUE(restored.isBlacklisted("com.obsproject.Studio"));
  EXPECT_FALSE(restored.removedDefaults().contains("obs"));
  restored.save();
  GameWhitelist reblocked;
  reblocked.load();
  EXPECT_TRUE(reblocked.isBlacklisted("com.obsproject.Studio"));
}

TEST_F(GameWhitelistTest, DefaultAliasDoesNotRewriteWhitelistOrCustomIdentity) {
  wl->addToWhitelist("com.obsproject.Studio", "Custom title");
  EXPECT_TRUE(wl->isWhitelisted("com.obsproject.Studio"));
  EXPECT_FALSE(wl->isWhitelisted("obs"));
  wl->addToBlacklist("exe:/games/obsidian/game");
  EXPECT_TRUE(wl->isBlacklisted("exe:/games/obsidian/game"));
  EXPECT_FALSE(wl->isBlacklisted("exe:/games/obsidian/game2"));
  EXPECT_EQ(wl->getWhitelistName("com.obsproject.Studio"), "Custom title");
}

TEST_F(GameWhitelistTest, LegacyAliasVetoesAndRemovalMarkersMigrateOnLoad) {
  const QString path = configHome.path() + "/trophy/game-whitelist.json";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  QFile file(path);
  ASSERT_TRUE(file.open(QIODevice::WriteOnly));
  file.write(R"({"blacklist":["com.obsproject.Studio","obs-studio"],
                  "removedDefaults":["com.obsproject.Studio"]})");
  file.close();
  wl->load();
  EXPECT_FALSE(wl->isBlacklisted("obs"));
  EXPECT_FALSE(wl->isBlacklisted("com.obsproject.Studio"));
  EXPECT_FALSE(wl->blacklistEntries().contains("obs-studio"));
  EXPECT_TRUE(wl->removedDefaults().contains("obs"));
}

TEST_F(GameWhitelistTest, DuplicateDefaultsMergeAndPreserveEitherUnblockChoice) {
  const QList<QPair<QString, QString>> groups{
      {"obs", "obs64"}, {"signal", "signal-desktop"},
      {"telegram", "telegram-desktop"}, {"vkcube", "vkcube-wayland"}};
  const QString path = configHome.path() + "/trophy/game-whitelist.json";
  ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  for (const auto &[canonical, legacy] : groups) {
    SCOPED_TRACE(legacy.toStdString());
    EXPECT_FALSE(GameWhitelist::defaultBlacklistEntries().contains(legacy));
    // Legacy lists can contain both rows; load coalesces them into one veto.
    for (const QString &removed : {QString(), canonical, legacy}) {
      QFile file(path);
      ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      file.write(QJsonDocument(QJsonObject{
          {"blacklist", QJsonArray{canonical, legacy}},
          {"removedDefaults", removed.isEmpty() ? QJsonArray{} : QJsonArray{removed}},
          {"whitelist", QJsonObject{{legacy, "User chosen title"}}}}).toJson());
      file.close();
      GameWhitelist restored;
      restored.load();
      EXPECT_EQ(restored.blacklistEntries().count(canonical), removed.isEmpty() ? 1 : 0);
      EXPECT_FALSE(restored.blacklistEntries().contains(legacy));
      EXPECT_EQ(restored.isBlacklisted(legacy), removed.isEmpty());
      EXPECT_EQ(restored.isBlacklisted(canonical), removed.isEmpty());
      // Consolidating veto rows never migrates an identity/approval key.
      EXPECT_EQ(restored.getWhitelistName(legacy), "User chosen title");
      restored.save();
      GameWhitelist roundTrip;
      roundTrip.load();
      EXPECT_EQ(roundTrip.isBlacklisted(legacy), removed.isEmpty());
      EXPECT_FALSE(roundTrip.blacklistEntries().contains(legacy));
    }
  }
}

TEST_F(GameWhitelistTest, RemovedDefaultsPersistAcrossReload) {
  // Allowing a built-in (remove from blacklist) survives save/load:
  // the constructor must not resurrect it.
  ASSERT_TRUE(wl->isBlacklisted("xalia"));
  wl->removeFromBlacklist("Xalia.EXE");
  EXPECT_FALSE(wl->isBlacklisted("xalia"));
  wl->save();

  auto wl2 = std::make_unique<GameWhitelist>();
  wl2->load();
  EXPECT_FALSE(wl2->isBlacklisted("xalia"));
  // ...while untouched built-ins still seed.
  EXPECT_TRUE(wl2->isBlacklisted("steam"));

  // Re-blocking clears the marker: present again after reload.
  wl2->addToBlacklist("xalia");
  wl2->save();
  auto wl3 = std::make_unique<GameWhitelist>();
  wl3->load();
  EXPECT_TRUE(wl3->isBlacklisted("xalia"));
}

TEST_F(GameWhitelistTest, PrettySystemNamesCoverAllDefaults) {
  // Every built-in key renders a non-empty friendly label (some apps
  // style themselves lowercase: mpv, kitty, foot).
  for (const QString &key : GameWhitelist::defaultBlacklistEntries())
    EXPECT_FALSE(GameWhitelist::prettySystemName(key).isEmpty());
  EXPECT_EQ(GameWhitelist::prettySystemName("steam"), "Steam");
  EXPECT_EQ(GameWhitelist::prettySystemName("vkcube"), "Vulkan Cube (test app)");
  EXPECT_EQ(GameWhitelist::prettySystemName("whatever"), "whatever");
}

TEST_F(GameWhitelistTest, PrettySystemNames) {
  EXPECT_EQ(GameWhitelist::prettySystemName("steam"), "Steam");
  EXPECT_EQ(GameWhitelist::prettySystemName("discord"), "Discord");
  EXPECT_EQ(GameWhitelist::prettySystemName("obs"), "OBS");
  EXPECT_EQ(GameWhitelist::prettySystemName("whatever"), "whatever");
}

TEST_F(GameWhitelistTest, BlacklistHasBuiltInDefaults) {
  // Constructor pre-populates well-known non-games so the detector
  // ignores launchers, chat apps, and test tools out of the box.
  QStringList entries = wl->blacklistEntries();
  EXPECT_FALSE(entries.isEmpty());
  EXPECT_TRUE(wl->isBlacklisted("steam"));
  EXPECT_TRUE(wl->isBlacklisted("discord"));
  EXPECT_TRUE(wl->isBlacklisted("obs"));
  EXPECT_TRUE(wl->isBlacklisted("vkcube"));
}

// --- Whitelist CRUD ---

TEST_F(GameWhitelistTest, AddToWhitelist) {
  wl->addToWhitelist("steam_app_12345", "Counter-Strike 2");
  EXPECT_TRUE(wl->isWhitelisted("steam_app_12345"));
}

TEST_F(GameWhitelistTest, RemoveFromWhitelist) {
  wl->addToWhitelist("steam_app_12345");
  wl->removeFromWhitelist("steam_app_12345");
  EXPECT_FALSE(wl->isWhitelisted("steam_app_12345"));
}

TEST_F(GameWhitelistTest, WhitelistDisplayName) {
  wl->addToWhitelist("steam_app_730", "CS2");
  EXPECT_EQ(wl->getWhitelistName("steam_app_730"), "CS2");
}

TEST_F(GameWhitelistTest, WhitelistDisplayNameEmpty) {
  wl->addToWhitelist("steam_app_730");
  // When no display name is provided, should return empty or the windowClass
  QString name = wl->getWhitelistName("steam_app_730");
  // Just verify it doesn't crash; exact behavior depends on impl
  Q_UNUSED(name);
  EXPECT_TRUE(wl->isWhitelisted("steam_app_730"));
}

TEST_F(GameWhitelistTest, WhitelistEntriesListing) {
  wl->addToWhitelist("game1");
  wl->addToWhitelist("game2");
  wl->addToWhitelist("game3");

  QStringList entries = wl->whitelistEntries();
  EXPECT_EQ(entries.size(), 3);
  EXPECT_TRUE(entries.contains("game1"));
  EXPECT_TRUE(entries.contains("game2"));
  EXPECT_TRUE(entries.contains("game3"));
}

TEST_F(GameWhitelistTest, NotWhitelistedByDefault) {
  EXPECT_FALSE(wl->isWhitelisted("random_app"));
}

TEST_F(GameWhitelistTest, RemoveNonExistentWhitelistNoError) {
  // Should not crash or error
  wl->removeFromWhitelist("nonexistent");
  EXPECT_FALSE(wl->isWhitelisted("nonexistent"));
}

// --- Blacklist CRUD ---

TEST_F(GameWhitelistTest, AddToBlacklist) {
  wl->addToBlacklist("obs-studio");
  EXPECT_TRUE(wl->isBlacklisted("obs-studio"));
}

TEST_F(GameWhitelistTest, RemoveFromBlacklist) {
  wl->addToBlacklist("obs-studio");
  wl->removeFromBlacklist("obs-studio");
  EXPECT_FALSE(wl->isBlacklisted("obs-studio"));
}

TEST_F(GameWhitelistTest, BlacklistEntriesListing) {
  const int initialSize = wl->blacklistEntries().size();
  wl->addToBlacklist("app1");
  wl->addToBlacklist("app2");

  QStringList entries = wl->blacklistEntries();
  EXPECT_EQ(entries.size(), initialSize + 2);
  EXPECT_TRUE(entries.contains("app1"));
  EXPECT_TRUE(entries.contains("app2"));
}

TEST_F(GameWhitelistTest, NotBlacklistedByDefault) {
  EXPECT_FALSE(wl->isBlacklisted("random_app"));
}

// --- Save/Load Persistence ---

TEST_F(GameWhitelistTest, SaveLoadRoundTrip) {
  wl->addToWhitelist("steam_app_730", "CS2");
  wl->addToWhitelist("proton_game", "Elden Ring");
  wl->addToBlacklist("obs-studio");
  wl->addToBlacklist("lutris-wrapper");
  wl->save();

  // Create a fresh instance and load
  auto wl2 = std::make_unique<GameWhitelist>();
  wl2->load();

  EXPECT_TRUE(wl2->isWhitelisted("steam_app_730"));
  EXPECT_TRUE(wl2->isWhitelisted("proton_game"));
  EXPECT_EQ(wl2->getWhitelistName("steam_app_730"), "CS2");
  EXPECT_EQ(wl2->getWhitelistName("proton_game"), "Elden Ring");
  EXPECT_TRUE(wl2->isBlacklisted("obs-studio"));
  EXPECT_TRUE(wl2->isBlacklisted("lutris-wrapper"));
}

TEST_F(GameWhitelistTest, LoadNonExistentFile) {
  // Should not crash; whitelist stays empty, built-in blacklist stays intact.
  wl->load();
  EXPECT_TRUE(wl->whitelistEntries().isEmpty());
  EXPECT_TRUE(wl->isBlacklisted("steam"));
}

TEST_F(GameWhitelistTest, PlaceholderReporterKeyPredicate) {
  EXPECT_TRUE(GameWhitelist::isPlaceholderReporterKey("Unknown Vulkan Game"));
  EXPECT_TRUE(GameWhitelist::isPlaceholderReporterKey("unknown vulkan game"));
  EXPECT_TRUE(GameWhitelist::isPlaceholderReporterKey("Unknown Vulkan Game.exe"));
  EXPECT_FALSE(GameWhitelist::isPlaceholderReporterKey("steam_app_1422450"));
  EXPECT_FALSE(GameWhitelist::isPlaceholderReporterKey("Deadlock"));
  EXPECT_FALSE(GameWhitelist::isPlaceholderReporterKey(""));
}

TEST_F(GameWhitelistTest, PlaceholderWhitelistEntryRepairedOnLoad) {
  // Regression: the authorize path used to whitelist the Vulkan null-name
  // placeholder under whichever game authorized last, aliasing every
  // unidentified reporter to that game. Loading such a file must drop the
  // mapping and restore the built-in default blacklist entry.
  ASSERT_TRUE(QDir().mkpath(configHome.path() + "/trophy"));
  QFile legacy(configHome.path() + "/trophy/game-whitelist.json");
  ASSERT_TRUE(legacy.open(QIODevice::WriteOnly));
  legacy.write(R"({"whitelist":{"unknown vulkan game":"Some Game"},
                   "removedDefaults":["unknown vulkan game"]})");
  legacy.close();

  auto reloaded = std::make_unique<GameWhitelist>();
  reloaded->load();
  EXPECT_FALSE(reloaded->isWhitelisted("unknown vulkan game"));
  EXPECT_TRUE(reloaded->isBlacklisted("unknown vulkan game"));
  EXPECT_FALSE(
      reloaded->removedDefaults().contains("unknown vulkan game"));
}

TEST_F(GameWhitelistTest, PlaceholderCannotBeWhitelistedThroughCrud) {
  wl->addToWhitelist("Unknown Vulkan Game", "First Game");
  EXPECT_FALSE(wl->isWhitelisted("unknown vulkan game"));
}

// --- Mixed operations ---

TEST_F(GameWhitelistTest, WhitelistAndBlacklistIndependent) {
  wl->addToWhitelist("app1");
  wl->addToBlacklist("app2");

  EXPECT_TRUE(wl->isWhitelisted("app1"));
  EXPECT_FALSE(wl->isBlacklisted("app1"));

  EXPECT_FALSE(wl->isWhitelisted("app2"));
  EXPECT_TRUE(wl->isBlacklisted("app2"));
}

TEST_F(GameWhitelistTest, MultipleAddsSameEntry) {
  wl->addToWhitelist("game1", "Name1");
  wl->addToWhitelist("game1", "Name2"); // Should update the name

  // Should still have only one entry
  QStringList entries = wl->whitelistEntries();
  EXPECT_EQ(entries.count("game1"), 1);
}
