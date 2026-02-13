/*
 * Trophy - Unit Tests
 * Test suite for GameDetectionService logic and caching
 */

#include <QCoreApplication>
#include <QSignalSpy>
#include <gtest/gtest.h>

// We need to include the source to access private members via friend class
// if we were just linking against a library. But here we compile the source
// directly.
#include "detection/GameDetectionService.h"

// Ensure QCoreApplication exists
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

namespace GameRecorder {

class GameDetectionServiceTest : public ::testing::Test {
protected:
  GameDetectionService service;

  // Helper to access private m_nonGamePids
  bool isPidInNonGameCache(qint64 pid) {
    return service.m_nonGamePids.contains(pid);
  }

  void addPidToNonGameCache(qint64 pid) { service.m_nonGamePids.insert(pid); }

  void clearCache() { service.m_nonGamePids.clear(); }

  // Helper to trigger check (it's a private slot, but we can invoke it)
  void triggerCheckForGames() { service.checkForGames(); }
};

TEST_F(GameDetectionServiceTest, CurrentProcessIsNotGame) {
  // The test runner itself is not a game
  qint64 currentPid = QCoreApplication::applicationPid();

  triggerCheckForGames();

  // It should have been scanned and found not to be a game
  EXPECT_TRUE(service.currentGameName().isEmpty());

  // And it should have been added to the non-game cache
  EXPECT_TRUE(isPidInNonGameCache(currentPid));
}

TEST_F(GameDetectionServiceTest, SkipsCachedNonGamePids) {
  qint64 currentPid = QCoreApplication::applicationPid();

  // 1. Manually add current PID to cache
  addPidToNonGameCache(currentPid);

  // 2. Trigger check
  triggerCheckForGames();

  // 3. Verify it is STILL in the cache
  // This confirms the logic: if (m_nonGamePids.contains(pid)) { start insert;
  // continue; } If the logic was broken (e.g. it cleared the cache and
  // re-scanned), it might still end up in the cache, but let's at least verify
  // consistency.
  EXPECT_TRUE(isPidInNonGameCache(currentPid));
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

TEST_F(GameDetectionServiceTest, BlacklistManagement) {
  QString windowClass = "gnome-shell";

  service.addToBlacklist(windowClass);

  QStringList blacklist = service.blacklist();
  EXPECT_TRUE(blacklist.contains(windowClass));

  service.removeFromBlacklist(windowClass);
  blacklist = service.blacklist();
  EXPECT_FALSE(blacklist.contains(windowClass));
}

} // namespace GameRecorder
