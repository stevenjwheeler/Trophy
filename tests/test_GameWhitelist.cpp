/*
 * Trophy - Unit Tests
 * Test suite for GameWhitelist CRUD and persistence
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "detection/GameWhitelist.h"

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
