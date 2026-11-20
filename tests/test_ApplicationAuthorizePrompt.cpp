/* Regression: phantom "New Game" prompts for already-approved games.
 * The Vulkan null-name placeholder ("Unknown Vulkan Game") aliased every
 * unidentified reporter to one whitelisted game, and the prompt paths
 * never checked restore tokens. Application is constructed but never
 * initialized: no services, portal, socket or QML engine are started.
 * Only the drop paths are exercised here — the show path needs a real
 * notification daemon and is covered by inspection.
 */
#include "core/Application.h"
#include "core/RecordingManager.h"
#include "detection/GameDetectionService.h"
#include "settings/SettingsManager.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <gtest/gtest.h>

using namespace TrophyBackend;

namespace TrophyBackend {
class ApplicationAuthorizePromptTest : public ::testing::Test {
protected:
  Application &app = *static_cast<Application *>(QApplication::instance());

  void SetUp() override {
    // Uninitialized app owns nothing: wire the exact members the prompt
    // paths read. XDG isolation comes from main(); the detector never
    // listens and the notification service is never touched on drop
    // paths (deliberately left null so a send would crash).
    app.m_settingsManager = std::make_unique<SettingsManager>(&app);
    app.m_gameDetector = std::make_unique<GameDetectionService>(&app);
    app.m_pendingAutoRecordGame.clear();
    app.m_pendingDvrGame.clear();
    app.m_pendingWindowGame.clear();
    app.m_pendingAudioGame.clear();
    app.m_pendingCaptureKey.clear();
    app.m_windowWaitTimer = new QTimer(&app);
    app.m_audioWaitTimer = new QTimer(&app);
    app.m_detectionDebounceTimer = new QTimer(&app);
    QObject::connect(app.m_gameDetector.get(), &GameDetectionService::currentGameChanged,
                     &app, &Application::resumePendingCapture);
    setCurrent("Brand New Game");
  }

  void TearDown() override {
    delete app.m_windowWaitTimer;
    app.m_windowWaitTimer = nullptr;
    delete app.m_audioWaitTimer;
    app.m_audioWaitTimer = nullptr;
    delete app.m_detectionDebounceTimer;
    app.m_detectionDebounceTimer = nullptr;
    ++app.m_audioWaitGeneration;
    app.m_audioProbeTicket = 0;
    app.m_audioProbeInflight = false;
    app.m_settingsManager.reset();
    app.m_gameDetector.reset();
    app.m_pendingAutoRecordGame.clear();
    app.m_pendingDvrGame.clear();
    app.m_pendingWindowGame.clear();
  }

  // Friendship is not inherited by TEST_F bodies: every private touch
  // goes through these fixture helpers.
  bool gate(const QString &game) { return app.shouldShowAuthorizePrompt(game); }
  void setCurrent(const QString &game, qint64 pid = QCoreApplication::applicationPid()) {
    app.m_gameDetector->updateCurrentGame(game, pid, game);
  }
  void setCurrentIdentity(const QString &name, const QString &key,
                          qint64 pid = QCoreApplication::applicationPid()) {
    app.m_gameDetector->updateCurrentGame(name, pid, key);
  }
  QString token(const QString &key) const { return app.m_settingsManager->getRestoreToken(key); }
  void setToken(const QString &game, const QString &token) {
    app.m_settingsManager->setRestoreToken(game, token);
  }
  void clearToken(const QString &game) {
    app.m_settingsManager->removeRestoreToken(game);
  }
  void block(const QString &game) {
    // No return-value assert: the veto persists in the isolated config
    // across tests, so re-blocking is a no-op false. gate() below proves
    // the veto landed.
    app.m_gameDetector->addToBlacklist(game);
  }
  void armRecordPending(const QString &game) {
    app.m_pendingAutoRecordGame = game;
  }
  void armDvrPending(const QString &game) { app.m_pendingDvrGame = game; }
  void armWindowPending(const QString &game, bool isRecord) {
    app.m_pendingWindowGame = game;
    app.m_pendingWindowIsRecord = isRecord;
    app.m_pendingCaptureKey = app.m_gameDetector->currentGameKey();
  }
  void armAudioPending(const QString &game, bool isRecord) {
    app.m_pendingAudioGame = game;
    app.m_pendingAudioIsRecord = isRecord;
    app.m_pendingCaptureKey = app.m_gameDetector->currentGameKey();
  }
  void resume() { app.resumePendingCapture(); }
  void startPending(const QString &game) { app.startWhenWindowReady(game, true); }
  bool windowTimerActive() const { return app.m_windowWaitTimer->isActive(); }
  bool windowIsRecord() const { return app.m_pendingWindowIsRecord; }
  bool audioPendingEmpty() const { return app.m_pendingAudioGame.isEmpty(); }
  bool audioRetryActive() const {
    return app.m_audioWaitTimer->isActive() || app.m_audioProbeInflight;
  }
  bool audioIsRecord() const { return app.m_pendingAudioIsRecord; }
  void fireDebounce() { app.onAuthorizeDebounceFired(); }
  void fireWindowTimeout() { app.onWindowWaitTimeout(); }
  void authorize(const QString &game) { app.authorizeGame(game); }
  bool recordPendingEmpty() const {
    return app.m_pendingAutoRecordGame.isEmpty();
  }
  bool dvrPendingEmpty() const { return app.m_pendingDvrGame.isEmpty(); }
  bool windowPendingEmpty() const { return app.m_pendingWindowGame.isEmpty(); }
  void finishDuringRecovery() {
    app.m_recordingManager = std::make_unique<RecordingManager>();
    app.m_recordingManager->m_captureRecoveryPending = true;
    // Replay is deliberately null: resuming it here would fail immediately.
    app.resumeReplayAfterRecording();
    EXPECT_TRUE(app.m_pendingWindowGame.isEmpty());
    EXPECT_TRUE(app.m_pendingAudioGame.isEmpty());
    app.m_recordingManager->m_captureRecoveryPending = false;
    app.m_recordingManager.reset();
  }
  void clearWindowPending() {
    app.m_pendingWindowGame.clear();
    app.m_windowWaitTimer->stop();
    app.m_pendingAudioGame.clear();
    app.m_audioWaitTimer->stop();
    ++app.m_audioWaitGeneration;
  }
  bool whitelisted(const QString &key) const {
    return app.gameDetector()->whitelist().contains(key);
  }
  void approvedDetectionArmsRecord(bool alreadyListed) {
    const QString name = "Approved Autostart Fixture";
    const QString key = "steam_app_1973530";
    auto &detector = *app.m_gameDetector;
    detector.updateCurrentGame({}, 0);
    if (alreadyListed)
      detector.addToWhitelist(key, name);
    else
      detector.removeFromWhitelist(key);
    auto process = ProcessCensus::inspect(QCoreApplication::applicationPid());
    ASSERT_GT(process.pid, 0);
    process.steamId = 1973530;
    detector.m_steam = {
        {1973530, name, QFileInfo(process.payload()).absolutePath()}};
    app.m_settingsManager->setAutoRecordEnabled(true);
    app.m_settingsManager->setInstantReplayEnabled(false);
    app.m_settingsManager->setRestoreTokenForIdentity(key, name,
                                                      "approved-token");
    QSignalSpy detected(&detector, &GameDetectionService::gameDetected);
    QObject::connect(&detector, &GameDetectionService::gameDetected, &app,
                     &Application::onGameDetected);
    // Exercises the real production callback: authorization may reidentify
    // and destroy the very instance from which gameDetected was emitted.
    detector.apply({{process.pid, process}});
    ASSERT_EQ(detected.size(), 1);
    EXPECT_EQ(app.m_pendingCaptureKey, key);
    EXPECT_EQ(app.m_pendingAudioGame, name);
    EXPECT_TRUE(app.m_pendingAudioIsRecord);
    EXPECT_EQ(detector.currentGameName(), name);
    EXPECT_TRUE(detector.whitelist().contains(key));
    EXPECT_FALSE(app.m_detectionDebounceTimer->isActive());
    EXPECT_EQ(app.m_settingsManager->getRestoreTokenForIdentity(key, name),
              "approved-token");
  }
};
} // namespace TrophyBackend

TEST_F(ApplicationAuthorizePromptTest, FreshGameMayPrompt) {
  EXPECT_TRUE(gate("Brand New Game"));
  EXPECT_FALSE(gate(""));
}

TEST_F(ApplicationAuthorizePromptTest,
       ApprovedDetectionArmsRecordAfterWhitelistRebuild) {
  approvedDetectionArmsRecord(false);
}

TEST_F(ApplicationAuthorizePromptTest,
       ApprovedListedDetectionArmsRecordWithoutReapproval) {
  approvedDetectionArmsRecord(true);
}

TEST_F(ApplicationAuthorizePromptTest, ApprovedGameNeverPrompts) {
  setCurrent("Approved Game");
  setToken("Approved Game", "token-bytes");
  EXPECT_FALSE(gate("Approved Game"));
  clearToken("Approved Game");
  EXPECT_TRUE(gate("Approved Game"));
}

TEST_F(ApplicationAuthorizePromptTest, BlockedGameNeverPrompts) {
  setCurrent("Blocked Game");
  block("Blocked Game");
  EXPECT_FALSE(gate("Blocked Game"));
}

TEST_F(ApplicationAuthorizePromptTest, DebounceDropsApprovedPending) {
  setCurrent("Approved Game");
  setToken("Approved Game", "token-bytes");
  armRecordPending("Approved Game");
  fireDebounce();
  EXPECT_TRUE(recordPendingEmpty());
  EXPECT_TRUE(dvrPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest,
       ProcessIdentifiedGameNeedsNoWindowForApprovalEligibility) {
  setCurrent("Windowless Game");
  armRecordPending("Windowless Game");
  EXPECT_TRUE(gate("Windowless Game"));
  EXPECT_FALSE(recordPendingEmpty());
  setToken("Windowless Game", "new-approval");
  fireDebounce();
  EXPECT_TRUE(recordPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest, DebounceRetiresDeadWindowlessRequest) {
  setCurrent("Exited Game", 2147483647);
  armDvrPending("Exited Game");
  fireDebounce();
  EXPECT_TRUE(dvrPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest, DebounceDropsBlockedPending) {
  block("Blocked Game");
  armDvrPending("Blocked Game");
  fireDebounce();
  EXPECT_TRUE(dvrPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest, WindowWaitRetainsApprovedLiveGame) {
  setCurrent("Approved Game");
  setToken("Approved Game", "token-bytes");
  armWindowPending("Approved Game", true);
  fireWindowTimeout();
  // A slow launch keeps waiting for compositor snapshots, never a picker.
  EXPECT_FALSE(windowPendingEmpty());
  EXPECT_TRUE(recordPendingEmpty());
  EXPECT_TRUE(dvrPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest,
       ProcessHandoffRestartsPendingAudioWithoutWindowGate) {
  setCurrentIdentity("Slow Game", "steam_app_610102", 2147483647);
  armAudioPending("Slow Game", true);
  // Same identity, new live PID: no gameDetected, but the pending start resumes.
  setCurrentIdentity("Slow Game", "steam_app_610102");
  EXPECT_FALSE(audioPendingEmpty());
  EXPECT_TRUE(windowPendingEmpty());
  EXPECT_TRUE(audioRetryActive());
  EXPECT_TRUE(audioIsRecord());
}

TEST_F(ApplicationAuthorizePromptTest, ProcessHandoffPreservesDvrIntent) {
  setCurrentIdentity("Slow Game", "steam_app_610102", 2147483647);
  armWindowPending("Slow Game", false);
  setCurrentIdentity("Slow Game", "steam_app_610102");
  EXPECT_FALSE(audioPendingEmpty());
  EXPECT_TRUE(audioRetryActive());
  EXPECT_FALSE(audioIsRecord());
}

TEST_F(ApplicationAuthorizePromptTest, AlreadyExitedLauncherKeepsApprovedStartPending) {
  setCurrentIdentity("Slow Game", "steam_app_610102", 2147483647);
  setToken("steam_app_610102", "saved-token");
  startPending("Slow Game");
  EXPECT_FALSE(windowPendingEmpty());
  fireWindowTimeout();
  EXPECT_FALSE(windowPendingEmpty());
  setCurrentIdentity("Slow Game", "steam_app_610102");
  EXPECT_TRUE(audioRetryActive());
}

TEST_F(ApplicationAuthorizePromptTest, HandoffWithoutPendingIntentDoesNotStartCapture) {
  setCurrentIdentity("Slow Game", "steam_app_610102", 2147483647);
  setCurrentIdentity("Slow Game", "steam_app_610102");
  EXPECT_TRUE(windowPendingEmpty());
  EXPECT_TRUE(audioPendingEmpty());
  EXPECT_FALSE(windowTimerActive());
}

TEST_F(ApplicationAuthorizePromptTest, PendingHandoffCannotFollowNamesakeIdentity) {
  setCurrentIdentity("Same Name", "exe:/first/game");
  armAudioPending("Same Name", true);
  setCurrentIdentity("Same Name", "exe:/second/game");
  resume();
  EXPECT_TRUE(windowPendingEmpty());
  EXPECT_FALSE(windowTimerActive());
}

TEST_F(ApplicationAuthorizePromptTest, OldGameCannotPromptAfterReplacement) {
  armRecordPending("Old Game");
  setCurrent("New Game");
  EXPECT_FALSE(gate("Old Game"));
  EXPECT_TRUE(gate("New Game"));
  fireDebounce();
  EXPECT_TRUE(recordPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest, ClosedGameWindowTimeoutCannotRearm) {
  armWindowPending("Old Game", true);
  setCurrent("");
  fireWindowTimeout();
  EXPECT_TRUE(windowPendingEmpty());
  EXPECT_TRUE(recordPendingEmpty());
}

TEST_F(ApplicationAuthorizePromptTest, DeadReporterCannotPrompt) {
  setCurrent("Dead Game", 2147483647);
  EXPECT_FALSE(gate("Dead Game"));
}

TEST_F(ApplicationAuthorizePromptTest, AuthorizeSkipsPlaceholderKey) {
  // No game tracked, so the display name itself is the whitelist key.
  authorize("Unknown Vulkan Game");
  EXPECT_FALSE(whitelisted("unknown vulkan game"));
  // A real identity still whitelists normally.
  authorize("steam_app_610101");
  EXPECT_TRUE(whitelisted("steam_app_610101"));
}

TEST_F(ApplicationAuthorizePromptTest, LegacyApprovalMigratesWithoutApprovingNamesake) {
  setToken("Game.exe", "legacy-game-token");
  setCurrentIdentity("Game.exe", "exe:/games/first/game");
  EXPECT_FALSE(gate("Game.exe"));
  authorize("Game.exe");
  EXPECT_EQ(token("exe:/games/first/game"), "legacy-game-token");
  EXPECT_TRUE(token("Game.exe").isEmpty());
  setCurrentIdentity("Game.exe", "exe:/games/second/game");
  EXPECT_TRUE(gate("Game.exe"));
  setCurrentIdentity("Renamed Game", "exe:/games/first/game");
  EXPECT_FALSE(gate("Renamed Game"));
}

int main(int argc, char **argv) {
  const QString tempRoot = QDir("/tmp/opencode").exists()
                               ? QStringLiteral("/tmp/opencode") : QDir::tempPath();
  QTemporaryDir isolation(tempRoot + "/trophy-app-prompt-XXXXXX");
  if (!isolation.isValid())
    return 2;
  for (const char *key : {"XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
                          "XDG_RUNTIME_DIR"}) {
    const QString path = isolation.path() + "/" + key;
    if (!QDir().mkpath(path))
      return 2;
    if (!QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
      return 2;
    qputenv(key, path.toUtf8());
  }
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QT_QUICK_BACKEND", "software");
  Application app(argc, argv);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// Saving the first part during a window handoff must retain full recording,
// not resume the background DVR loop.
TEST_F(ApplicationAuthorizePromptTest, SavedHandoffPartCannotStartDvr) {
  setCurrentIdentity("Launcher Game", "steam_app_777");
  setToken("steam_app_777", "token-bytes");

  finishDuringRecovery();
}
