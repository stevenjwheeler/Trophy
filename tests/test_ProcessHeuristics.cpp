/*
 * Trophy - Unit Tests
 * Test suite for ProcessHeuristics game detection logic
 */

#include "detection/ProcessHeuristics.h"
#include <gtest/gtest.h>

using namespace GameRecorder;

class ProcessHeuristicsTest : public ::testing::Test {
protected:
  ProcessHeuristics heuristics;

  // Helper to build a ProcessInfo with specific fields
  ProcessHeuristics::ProcessInfo
  makeInfo(const QString &name = {}, const QString &windowClass = {},
           const QString &steamAppId = {}, const QString &cwd = {},
           bool isWine = false, bool isProton = false) {
    ProcessHeuristics::ProcessInfo info;
    info.name = name;
    info.windowClass = windowClass;
    info.steamAppId = steamAppId;
    info.cwd = cwd;
    info.isWine = isWine;
    info.isProton = isProton;
    return info;
  }
};

// --- gameConfidence() ---

TEST_F(ProcessHeuristicsTest, SteamGameHighConfidence) {
  auto info = makeInfo("game", "", "123456");
  float score = heuristics.gameConfidence(info);
  EXPECT_GE(score, 0.9f);
}

TEST_F(ProcessHeuristicsTest, WineBoostAddsScore) {
  auto baseline = makeInfo("unknown_app");
  auto withWine = makeInfo("unknown_app", "", "", "", true, false);

  float base = heuristics.gameConfidence(baseline);
  float boosted = heuristics.gameConfidence(withWine);
  EXPECT_GT(boosted, base);
}

TEST_F(ProcessHeuristicsTest, ProtonBoostAddsScore) {
  auto baseline = makeInfo("unknown_app");
  auto withProton = makeInfo("unknown_app", "", "", "", false, true);

  float base = heuristics.gameConfidence(baseline);
  float boosted = heuristics.gameConfidence(withProton);
  EXPECT_GT(boosted, base);
}

TEST_F(ProcessHeuristicsTest, KnownNonGameReturnsZero) {
  // These are explicitly in the non-game list
  QStringList nonGames = {"firefox", "discord", "spotify",
                          "chrome",  "code",    "vlc"};
  for (const auto &name : nonGames) {
    auto info = makeInfo(name);
    EXPECT_FLOAT_EQ(heuristics.gameConfidence(info), 0.0f)
        << "Expected 0.0 for known non-game: " << name.toStdString();
  }
}

TEST_F(ProcessHeuristicsTest, PatternMatchingSteamApp) {
  auto info = makeInfo("", "steam_app_12345");
  float score = heuristics.gameConfidence(info);
  EXPECT_GE(score, 0.5f);
}

TEST_F(ProcessHeuristicsTest, PatternMatchingEmulators) {
  QStringList emulators = {"retroarch", "dolphin-emu", "pcsx2", "rpcs3",
                           "yuzu"};
  for (const auto &name : emulators) {
    auto info = makeInfo(name);
    float score = heuristics.gameConfidence(info);
    EXPECT_GE(score, 0.5f) << "Expected >= 0.5 for emulator: "
                           << name.toStdString();
  }
}

TEST_F(ProcessHeuristicsTest, PathBasedDetectionSteamApps) {
  auto info = makeInfo("unknown_game", "", "",
                       "/home/user/.steam/steamapps/common/MyGame");
  float score = heuristics.gameConfidence(info);
  EXPECT_GE(score, 0.3f);
}

TEST_F(ProcessHeuristicsTest, PathBasedDetectionGamesDir) {
  auto info = makeInfo("unknown_game", "", "", "/home/user/Games/MyGame");
  float score = heuristics.gameConfidence(info);
  EXPECT_GE(score, 0.3f);
}

TEST_F(ProcessHeuristicsTest, PathBasedDetectionWinePrefix) {
  auto info = makeInfo("unknown_game", "", "",
                       "/home/user/.wine/drive_c/Program Files/MyGame");
  float score = heuristics.gameConfidence(info);
  EXPECT_GE(score, 0.3f);
}

TEST_F(ProcessHeuristicsTest, CombinedSignalsCappedAtOne) {
  // All signals at once: Steam + Wine + Proton + game pattern + game path
  auto info = makeInfo("steam_app_12345", "steam_app_12345", "123456",
                       "/home/user/.steam/steamapps/common/MyGame", true, true);
  float score = heuristics.gameConfidence(info);
  EXPECT_FLOAT_EQ(score, 1.0f);
}

TEST_F(ProcessHeuristicsTest, EmptyInfoReturnsLow) {
  ProcessHeuristics::ProcessInfo info;
  float score = heuristics.gameConfidence(info);
  EXPECT_LE(score, 0.1f);
}

// --- isProbablyGame() ---

TEST_F(ProcessHeuristicsTest, IsProbablyGameAboveThreshold) {
  // Steam game should be above 0.5
  auto info = makeInfo("game", "", "123456");
  EXPECT_TRUE(heuristics.isProbablyGame(info));
}

TEST_F(ProcessHeuristicsTest, IsProbablyGameBelowThreshold) {
  // Empty info should be below 0.5
  ProcessHeuristics::ProcessInfo info;
  EXPECT_FALSE(heuristics.isProbablyGame(info));
}

TEST_F(ProcessHeuristicsTest, IsProbablyGameNonGame) {
  auto info = makeInfo("firefox");
  EXPECT_FALSE(heuristics.isProbablyGame(info));
}

// Note: The heuristics are used for auto-record detection.
// Manual recording of any window (games or not) is supported by the app
// and does not go through this detection path.

// --- isKnownLauncher() (private, tested via gameConfidence behavior) ---
// We test the pattern lists indirectly through gameConfidence scores

TEST_F(ProcessHeuristicsTest, KnownNonGameCaseInsensitive) {
  auto info = makeInfo("Firefox");
  EXPECT_FLOAT_EQ(heuristics.gameConfidence(info), 0.0f);

  auto info2 = makeInfo("DISCORD");
  EXPECT_FLOAT_EQ(heuristics.gameConfidence(info2), 0.0f);
}

TEST_F(ProcessHeuristicsTest, UnknownProcessGetsNonZeroWithGamePath) {
  // A process we don't know about but running from a game path
  auto info = makeInfo("my_indie_game", "", "",
                       "/home/user/.steam/steamapps/common/IndieGame");
  float score = heuristics.gameConfidence(info);
  EXPECT_GT(score, 0.0f);
}
