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
  void SetUp() override { wl = std::make_unique<GameWhitelist>(); }

  void TearDown() override {
    wl.reset();
    // Clean up any saved config files
    QString configPath =
        QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    QDir configDir(configPath + "/trophy");
    if (configDir.exists()) {
      configDir.removeRecursively();
    }
  }

  std::unique_ptr<GameWhitelist> wl;
};

// --- Empty Default State ---

TEST_F(GameWhitelistTest, EmptyByDefault) {
  EXPECT_TRUE(wl->whitelistEntries().isEmpty());
  EXPECT_TRUE(wl->blacklistEntries().isEmpty());
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
  wl->addToBlacklist("app1");
  wl->addToBlacklist("app2");

  QStringList entries = wl->blacklistEntries();
  EXPECT_EQ(entries.size(), 2);
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
  // Should not crash, just result in empty lists
  wl->load();
  EXPECT_TRUE(wl->whitelistEntries().isEmpty());
  EXPECT_TRUE(wl->blacklistEntries().isEmpty());
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
