/*
 * Trophy - Unit Tests
 * Test suite for SettingsManager persistence and property system
 */

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <memory>

#include "settings/SettingsManager.h"

using namespace TrophyBackend;

class SettingsManagerTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      app = std::make_unique<QCoreApplication>(argc, argv);
    }
    // Redirect config path to temp for test isolation
    QCoreApplication::setOrganizationName("TrophyTest");
    QCoreApplication::setApplicationName("TrophyTestApp");
  }
  void TearDown() override {
    if (app) {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QCoreApplication::processEvents();
      app.reset();
    }
  }
private:
  std::unique_ptr<QCoreApplication> app;
};

static auto *env =
    testing::AddGlobalTestEnvironment(new SettingsManagerTestEnvironment);

class SettingsManagerTest : public ::testing::Test {
protected:
  QTemporaryDir configHome;
  QByteArray oldConfigHome;
  void SetUp() override {
    ASSERT_TRUE(configHome.isValid());
    oldConfigHome = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    mgr = std::make_unique<SettingsManager>();
  }

  void TearDown() override {
    // Clean up settings file if created
    QString path = mgr->settingsFilePath();
    QFile::remove(path);
    mgr.reset();
    if (oldConfigHome.isNull())
      qunsetenv("XDG_CONFIG_HOME");
    else
      qputenv("XDG_CONFIG_HOME", oldConfigHome);
  }

  std::unique_ptr<SettingsManager> mgr;
};

// --- Default Values ---

TEST_F(SettingsManagerTest, CustomizationDefaultsAndRoundTrip) {
  EXPECT_EQ(mgr->themeMode(), "dark");
  EXPECT_EQ(mgr->accentColor(), "rose");
  QSignalSpy modeChanged(mgr.get(), &SettingsManager::themeModeChanged);
  QSignalSpy accentChanged(mgr.get(), &SettingsManager::accentColorChanged);
  for (const QString &mode : {QString("charcoal"), QString("amoled"), QString("dark")}) {
    mgr->setThemeMode(mode);
    for (const QString &accent : {QString("blue"), QString("purple"), QString("teal"),
                                 QString("orange"), QString("cyan"), QString("pink"), QString("rose")}) {
      mgr->setAccentColor(accent);
      SettingsManager reloaded;
      reloaded.load();
      EXPECT_EQ(reloaded.themeMode(), mode);
      EXPECT_EQ(reloaded.accentColor(), accent);
    }
  }
  EXPECT_EQ(modeChanged.count(), 3);
  EXPECT_EQ(accentChanged.count(), 21);
  mgr->setThemeMode("dark");
  mgr->setAccentColor("rose");
  mgr->setThemeMode("invalid");
  mgr->setAccentColor("#ffffff");
  EXPECT_EQ(modeChanged.count(), 3);
  EXPECT_EQ(accentChanged.count(), 21);
}

TEST_F(SettingsManagerTest, CustomizationLoadFallbacksAndResetNotify) {
  mgr->setThemeMode("amoled");
  mgr->setAccentColor("teal");
  QSignalSpy modeChanged(mgr.get(), &SettingsManager::themeModeChanged);
  QSignalSpy accentChanged(mgr.get(), &SettingsManager::accentColorChanged);
  for (const auto &customization : {QJsonObject{}, QJsonObject{{"themeMode", "bad"},
                                                             {"accentColor", 42}}}) {
    QFile file(mgr->settingsFilePath());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"customization", customization}}).toJson());
    file.close();
    mgr->load();
    EXPECT_EQ(mgr->themeMode(), "dark");
    EXPECT_EQ(mgr->accentColor(), "rose");
  }
  EXPECT_EQ(modeChanged.count(), 2);
  EXPECT_EQ(accentChanged.count(), 2);
  mgr->setThemeMode("amoled");
  mgr->setAccentColor("purple");
  modeChanged.clear();
  accentChanged.clear();
  mgr->resetToDefaults();
  EXPECT_EQ(mgr->themeMode(), "dark");
  EXPECT_EQ(mgr->accentColor(), "rose");
  EXPECT_EQ(modeChanged.count(), 1);
  EXPECT_EQ(accentChanged.count(), 1);
  EXPECT_TRUE(mgr->metaObject()->property(mgr->metaObject()->indexOfProperty("themeMode")).hasNotifySignal());
  EXPECT_TRUE(mgr->metaObject()->property(mgr->metaObject()->indexOfProperty("accentColor")).isWritable());
}

TEST_F(SettingsManagerTest, IdentityTokenMigrationAndRenameKeepApproval) {
  mgr->setRestoreToken("Game.exe", "legacy-token");
  const QString first = "exe:/games/first/game";
  const QString second = "exe:/games/second/game";
  EXPECT_EQ(mgr->getRestoreTokenForIdentity(first, "Game.exe"), "legacy-token");
  QSignalSpy changed(mgr.get(), &SettingsManager::waylandRestoreTokensChanged);
  mgr->setRestoreTokenForIdentity(first, "Game.exe", "legacy-token");
  EXPECT_EQ(changed.count(), 1);
  EXPECT_TRUE(mgr->getRestoreToken("Game.exe").isEmpty());
  EXPECT_EQ(mgr->getRestoreTokenForIdentity(first, "Renamed Game"), "legacy-token");
  EXPECT_TRUE(mgr->getRestoreTokenForIdentity(second, "Game.exe").isEmpty());
  mgr->setRestoreTokenForIdentity(first, "Game.exe", "legacy-token");
  EXPECT_EQ(changed.count(), 1);
  SettingsManager reloaded;
  reloaded.load();
  EXPECT_EQ(reloaded.getRestoreToken(first), "legacy-token");
  EXPECT_TRUE(reloaded.getRestoreToken("Game.exe").isEmpty());
}

TEST_F(SettingsManagerTest, DefaultVideoBitrate) {
  EXPECT_EQ(mgr->videoBitrate(), 15000);
}

TEST_F(SettingsManagerTest, DefaultFps) { EXPECT_EQ(mgr->fps(), 60); }

TEST_F(SettingsManagerTest, DefaultEncoder) {
  EXPECT_EQ(mgr->encoder(), "auto");
}

TEST_F(SettingsManagerTest, DefaultContainerFormat) {
  EXPECT_EQ(mgr->containerFormat(), "mkv");
}

TEST_F(SettingsManagerTest, DefaultEncoderPreset) {
  EXPECT_EQ(mgr->encoderPreset(), "fast");
}

TEST_F(SettingsManagerTest, DefaultVideoResolution) {
  EXPECT_EQ(mgr->videoResolution(), "native");
}

TEST_F(SettingsManagerTest, DefaultAudioSettings) {
  EXPECT_EQ(mgr->gameAudioDevice(), "default");
  EXPECT_EQ(mgr->microphoneDevice(), "default");
  EXPECT_EQ(mgr->voiceChatApp(), "discord");
  EXPECT_TRUE(mgr->separateAudioTracks());
}

TEST_F(SettingsManagerTest, DefaultStorageSettings) {
  EXPECT_EQ(mgr->maxStorageSize(), 50LL * 1024 * 1024 * 1024);
  EXPECT_TRUE(mgr->autoDeleteOldest());
  EXPECT_FALSE(mgr->recordingsPath().isEmpty());
}

TEST_F(SettingsManagerTest, DefaultDetectionSettings) {
  EXPECT_TRUE(mgr->autoDetectGames());
  EXPECT_FALSE(mgr->autoRecordEnabled());
}

// --- Signal Emission ---

TEST_F(SettingsManagerTest, SetVideoBitrateEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::videoBitrateChanged);
  mgr->setVideoBitrate(20000);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(mgr->videoBitrate(), 20000);
}

TEST_F(SettingsManagerTest, SetFpsEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::fpsChanged);
  mgr->setFps(30);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(mgr->fps(), 30);
}

TEST_F(SettingsManagerTest, SetEncoderEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::encoderChanged);
  mgr->setEncoder("nvenc_h264");
  EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsManagerTest, SetSeparateAudioTracksEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::separateAudioTracksChanged);
  mgr->setSeparateAudioTracks(false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(mgr->separateAudioTracks());
}

// Live-apply wiring depends on these three signals (Application routes
// them to the running pipeline's hot-swap).
TEST_F(SettingsManagerTest, SetMicrophoneDeviceEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::microphoneDeviceChanged);
  mgr->setMicrophoneDevice("easyeffects_source");
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(mgr->microphoneDevice(), "easyeffects_source");
}

TEST_F(SettingsManagerTest, SetGameAudioDeviceEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::gameAudioDeviceChanged);
  mgr->setGameAudioDevice("easyeffects_sink");
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(mgr->gameAudioDevice(), "easyeffects_sink");
}

TEST_F(SettingsManagerTest, SetVoiceChatAppEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::voiceChatAppChanged);
  mgr->setVoiceChatApp("teamspeak");
  EXPECT_EQ(spy.count(), 1);
  EXPECT_EQ(mgr->voiceChatApp(), "teamspeak");
}

// Notification options (all default on; round-trip through the setters).
TEST_F(SettingsManagerTest, NotificationTogglesDefaultOn) {
  EXPECT_TRUE(mgr->notifyRecordings());
  EXPECT_TRUE(mgr->notifyClips());
  EXPECT_TRUE(mgr->notifyStorage());
}

TEST_F(SettingsManagerTest, SetNotifyRecordingsEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::notifyRecordingsChanged);
  mgr->setNotifyRecordings(false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(mgr->notifyRecordings());
}

TEST_F(SettingsManagerTest, SetNotifyClipsEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::notifyClipsChanged);
  mgr->setNotifyClips(false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(mgr->notifyClips());
}

TEST_F(SettingsManagerTest, SetNotifyStorageEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::notifyStorageChanged);
  mgr->setNotifyStorage(false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(mgr->notifyStorage());
}

TEST_F(SettingsManagerTest, CompletionSoundDefaultsOnAndEmitsSignal) {
  EXPECT_TRUE(mgr->completionSoundEnabled());
  QSignalSpy spy(mgr.get(), &SettingsManager::completionSoundEnabledChanged);
  mgr->setCompletionSoundEnabled(false);
  EXPECT_EQ(spy.count(), 1);
  EXPECT_FALSE(mgr->completionSoundEnabled());
}

TEST_F(SettingsManagerTest, LegacyClipPingKeyHonouredOnLoad) {
  // Pre-rename settings only have notifications.clipPing; it must still
  // gate both chimes until the first save rewrites it.
  const QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());
  QJsonObject notifications;
  notifications["clipPing"] = false;
  QJsonObject root;
  root["notifications"] = notifications;
  QFile f(filePath);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write(QJsonDocument(root).toJson());
  f.close();

  mgr->load();
  EXPECT_FALSE(mgr->completionSoundEnabled());
}

TEST_F(SettingsManagerTest, CompletionSoundKeyWinsAndLegacyNotPersisted) {
  mgr->setCompletionSoundEnabled(false);
  mgr->save();
  QFile f(mgr->settingsFilePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  const QJsonObject notifications =
      QJsonDocument::fromJson(f.readAll()).object()["notifications"].toObject();
  EXPECT_TRUE(notifications.contains("completionSound"));
  EXPECT_FALSE(notifications.contains("clipPing"));
  EXPECT_FALSE(notifications["completionSound"].toBool(true));
}

TEST_F(SettingsManagerTest, SetAutoDetectGamesEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::autoDetectGamesChanged);
  mgr->setAutoDetectGames(false);
  EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsManagerTest, SetAutoRecordEnabledEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::autoRecordEnabledChanged);
  mgr->setAutoRecordEnabled(true);
  EXPECT_EQ(spy.count(), 1);
}

// --- No signal on same value ---

TEST_F(SettingsManagerTest, SetSameValueNoSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::videoBitrateChanged);
  // Set to same default value
  mgr->setVideoBitrate(15000);
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(SettingsManagerTest, SetSameFpsNoSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::fpsChanged);
  mgr->setFps(60); // Already default
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(SettingsManagerTest, SetSameEncoderNoSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::encoderChanged);
  mgr->setEncoder("auto"); // Already default
  EXPECT_EQ(spy.count(), 0);
}

TEST_F(SettingsManagerTest, DefaultWindowTrackingDisabled) {
  // Opt-in: nothing is installed until the user enables it in the wizard.
  EXPECT_FALSE(mgr->windowTrackingEnabled());
}

TEST_F(SettingsManagerTest, SetWindowTrackingEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::windowTrackingEnabledChanged);
  mgr->setWindowTrackingEnabled(true);
  EXPECT_TRUE(mgr->windowTrackingEnabled());
  EXPECT_EQ(spy.count(), 1);
  mgr->setWindowTrackingEnabled(true);
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, WindowTrackingPersistsRoundTrip) {
  mgr->setWindowTrackingEnabled(true);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_TRUE(mgr2->windowTrackingEnabled());
}

TEST_F(SettingsManagerTest, DefaultVulkanLayerDisabled) {
  EXPECT_FALSE(mgr->vulkanLayerEnabled());
}

TEST_F(SettingsManagerTest, SetVulkanLayerEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::vulkanLayerEnabledChanged);
  mgr->setVulkanLayerEnabled(true);
  EXPECT_TRUE(mgr->vulkanLayerEnabled());
  EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsManagerTest, VulkanLayerPersistsRoundTrip) {
  mgr->setVulkanLayerEnabled(true);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_TRUE(mgr2->vulkanLayerEnabled());
}

TEST_F(SettingsManagerTest, DefaultTrayBehaviour) {
  EXPECT_TRUE(mgr->minimizeToTray());
  EXPECT_FALSE(mgr->startMinimized());
}

TEST_F(SettingsManagerTest, SetTrayOptionsEmitSignals) {
  QSignalSpy minSpy(mgr.get(), &SettingsManager::minimizeToTrayChanged);
  mgr->setMinimizeToTray(false);
  EXPECT_FALSE(mgr->minimizeToTray());
  EXPECT_EQ(minSpy.count(), 1);
  mgr->setMinimizeToTray(false);
  EXPECT_EQ(minSpy.count(), 1); // same value: no repeat signal

  QSignalSpy startSpy(mgr.get(), &SettingsManager::startMinimizedChanged);
  mgr->setStartMinimized(true);
  EXPECT_TRUE(mgr->startMinimized());
  EXPECT_EQ(startSpy.count(), 1);
}

TEST_F(SettingsManagerTest, TrayOptionsPersistRoundTrip) {
  mgr->setMinimizeToTray(false);
  mgr->setStartMinimized(true);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_FALSE(mgr2->minimizeToTray());
  EXPECT_TRUE(mgr2->startMinimized());
}

TEST_F(SettingsManagerTest, DefaultStartOnLoginDisabled) {
  EXPECT_FALSE(mgr->startOnLogin());
}

TEST_F(SettingsManagerTest, SetStartOnLoginEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::startOnLoginChanged);
  mgr->setStartOnLogin(true);
  EXPECT_TRUE(mgr->startOnLogin());
  EXPECT_EQ(spy.count(), 1);
  mgr->setStartOnLogin(true);
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, StartOnLoginPersistsRoundTrip) {
  mgr->setStartOnLogin(true);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_TRUE(mgr2->startOnLogin());
}

TEST_F(SettingsManagerTest, DefaultInstantReplayEnabled) {
  EXPECT_TRUE(mgr->instantReplayEnabled());
}

TEST_F(SettingsManagerTest, SetInstantReplayEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::instantReplayEnabledChanged);
  mgr->setInstantReplayEnabled(false);
  EXPECT_FALSE(mgr->instantReplayEnabled());
  EXPECT_EQ(spy.count(), 1);
  mgr->setInstantReplayEnabled(false);
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, InstantReplayPersistsRoundTrip) {
  mgr->setInstantReplayEnabled(false);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_FALSE(mgr2->instantReplayEnabled());
}

TEST_F(SettingsManagerTest, DefaultGlobalShortcutsEnabled) {
  EXPECT_TRUE(mgr->globalShortcutsEnabled());
}

TEST_F(SettingsManagerTest, SetGlobalShortcutsEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::globalShortcutsEnabledChanged);
  mgr->setGlobalShortcutsEnabled(false);
  EXPECT_FALSE(mgr->globalShortcutsEnabled());
  EXPECT_EQ(spy.count(), 1);
  mgr->setGlobalShortcutsEnabled(false);
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, GlobalShortcutsPersistsRoundTrip) {
  mgr->setGlobalShortcutsEnabled(false);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_FALSE(mgr2->globalShortcutsEnabled());
}

TEST_F(SettingsManagerTest, DefaultReplayCachePathEmpty) {
  EXPECT_TRUE(mgr->replayCachePath().isEmpty());
}

TEST_F(SettingsManagerTest, SetReplayCachePathEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::replayCachePathChanged);
  mgr->setReplayCachePath("/tmp/trophy-replay");
  EXPECT_EQ(mgr->replayCachePath(), "/tmp/trophy-replay");
  EXPECT_EQ(spy.count(), 1);
  mgr->setReplayCachePath("/tmp/trophy-replay");
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, ReplayCachePathPersistsRoundTrip) {
  mgr->setReplayCachePath("/tmp/trophy-replay");
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_EQ(mgr2->replayCachePath(), "/tmp/trophy-replay");
}

TEST_F(SettingsManagerTest, ReplayCachePathIsQmlExposed) {
  // Regression: the getter/setter/signal existed without Q_PROPERTY, so
  // QML reads got `undefined` (startup warning) and writes from Settings
  // landed on a dynamic QObject property instead of the C++ member.
  const QMetaObject *mo = mgr->metaObject();
  const int idx = mo->indexOfProperty("replayCachePath");
  ASSERT_GE(idx, 0) << "replayCachePath missing from metaobject";
  const QMetaProperty prop = mo->property(idx);
  EXPECT_TRUE(prop.isWritable());
  EXPECT_TRUE(prop.hasNotifySignal());
  EXPECT_TRUE(mgr->setProperty("replayCachePath", "/tmp/qml-probe"));
  EXPECT_EQ(mgr->replayCachePath(), "/tmp/qml-probe");
  EXPECT_EQ(mgr->property("replayCachePath").toString(), "/tmp/qml-probe");
}

TEST_F(SettingsManagerTest, LibraryListViewDefaultsToGrid) {
  EXPECT_FALSE(mgr->libraryListView());
}

TEST_F(SettingsManagerTest, SetLibraryListViewEmitsSignal) {
  QSignalSpy spy(mgr.get(), &SettingsManager::libraryListViewChanged);
  mgr->setLibraryListView(true);
  EXPECT_TRUE(mgr->libraryListView());
  EXPECT_EQ(spy.count(), 1);
  mgr->setLibraryListView(true);
  EXPECT_EQ(spy.count(), 1); // same value: no repeat signal
}

TEST_F(SettingsManagerTest, LibraryListViewPersistsRoundTrip) {
  mgr->setLibraryListView(true);
  mgr->save();

  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();
  EXPECT_TRUE(mgr2->libraryListView());
}

TEST_F(SettingsManagerTest, LibraryListViewIsQmlExposed) {
  // Same regression class as replayCachePath: without Q_PROPERTY, QML
  // writes land on a dynamic property and reads get `undefined`.
  const QMetaObject *mo = mgr->metaObject();
  const int idx = mo->indexOfProperty("libraryListView");
  ASSERT_GE(idx, 0) << "libraryListView missing from metaobject";
  const QMetaProperty prop = mo->property(idx);
  EXPECT_TRUE(prop.isWritable());
  EXPECT_TRUE(prop.hasNotifySignal());
  EXPECT_TRUE(mgr->setProperty("libraryListView", true));
  EXPECT_TRUE(mgr->libraryListView());
  EXPECT_TRUE(mgr->property("libraryListView").toBool());
}

TEST_F(SettingsManagerTest, LegacySingularTokenKeyNotPersisted) {
  // The old "waylandRestoreToken" key (never mapped to a game) must not be
  // written anymore; only the per-game map persists. Note the map key
  // contains the singular as a substring, so compare parsed keys exactly.
  mgr->setRestoreToken("Deadlock", "tok123");
  mgr->save();
  QFile f(mgr->settingsFilePath());
  ASSERT_TRUE(f.open(QIODevice::ReadOnly));
  const QJsonDocument doc =
      QJsonDocument::fromJson(f.readAll());
  ASSERT_TRUE(doc.isObject());
  const QStringList keys = doc.object().keys();
  EXPECT_FALSE(keys.contains("waylandRestoreToken"));
  EXPECT_TRUE(keys.contains("waylandRestoreTokens"));
}

// --- Save/Load Round Trip ---

TEST_F(SettingsManagerTest, SaveLoadRoundTrip) {
  // Modify settings
  mgr->setVideoBitrate(25000);
  mgr->setFps(30);
  mgr->setEncoder("nvenc_h264");
  mgr->setContainerFormat("mp4");
  mgr->setSeparateAudioTracks(false);
  mgr->setAutoRecordEnabled(true);
  mgr->save();

  // Create a new instance and load
  auto mgr2 = std::make_unique<SettingsManager>();
  mgr2->load();

  EXPECT_EQ(mgr2->videoBitrate(), 25000);
  EXPECT_EQ(mgr2->fps(), 30);
  EXPECT_EQ(mgr2->encoder(), "nvenc_h264");
  EXPECT_EQ(mgr2->containerFormat(), "mp4");
  EXPECT_FALSE(mgr2->separateAudioTracks());
  EXPECT_TRUE(mgr2->autoRecordEnabled());
}

// --- Edge cases ---

TEST_F(SettingsManagerTest, LoadCorruptJson) {
  // Write invalid JSON to the settings file
  QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());

  QFile f(filePath);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write("this is { not valid json!!!");
  f.close();

  mgr->load();

  // Should fall back to defaults
  EXPECT_EQ(mgr->videoBitrate(), 15000);
  EXPECT_EQ(mgr->fps(), 60);
}

TEST_F(SettingsManagerTest, LoadPartialJson) {
  // Write JSON with only some fields
  QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());

  QJsonObject root;
  QJsonObject video;
  video["bitrate"] = 30000;
  // No fps, encoder, etc.
  root["video"] = video;

  QFile f(filePath);
  ASSERT_TRUE(f.open(QIODevice::WriteOnly));
  f.write(QJsonDocument(root).toJson());
  f.close();

  mgr->load();

  EXPECT_EQ(mgr->videoBitrate(), 30000); // From file
  EXPECT_EQ(mgr->fps(), 60);             // Default fallback
  EXPECT_EQ(mgr->encoder(), "auto");     // Default fallback
}

TEST_F(SettingsManagerTest, SettingsFilePath) {
  QString path = mgr->settingsFilePath();
  EXPECT_TRUE(path.endsWith("/settings.json"));
  EXPECT_TRUE(path.contains("trophy") || path.contains("TrophyTest"));
}

TEST_F(SettingsManagerTest, AvailableEncodersContainsAuto) {
  QStringList encoders = mgr->availableEncoders();
  EXPECT_TRUE(encoders.contains("auto"));
}

TEST_F(SettingsManagerTest, AvailableEncodersContainsX264) {
  QStringList encoders = mgr->availableEncoders();
  EXPECT_TRUE(encoders.contains("x264"));
}

TEST_F(SettingsManagerTest, ResetToDefaults) {
  mgr->setVideoBitrate(99999);
  mgr->setFps(144);
  mgr->resetToDefaults();

  EXPECT_EQ(mgr->videoBitrate(), 15000);
  EXPECT_EQ(mgr->fps(), 60);
}

TEST_F(SettingsManagerTest, ResetToDefaultsRestoresAllGaps) {
  // The P1 resetToDefaults gap fill: every persisted key must come back,
  // not just bitrate+fps — and the replay path clears while restore
  // tokens survive (tokens are consent evidence, never defaults).
  mgr->setVideoBitrate(99999);
  mgr->setFps(144);
  mgr->setEncoder("x264");
  mgr->setSeparateAudioTracks(false);
  mgr->setMicrophoneDevice("some-mic");
  mgr->setVoiceChatApp("discord");
  mgr->setMaxStorageSize(123456789);
  mgr->setAutoDetectGames(false);
  mgr->setAutoRecordEnabled(true);
  mgr->setReplayCachePath("/tmp/some-cache");
  mgr->setRestoreToken("Deadlock", "token-abc");
  mgr->resetToDefaults();

  EXPECT_EQ(mgr->videoBitrate(), 15000);
  EXPECT_EQ(mgr->fps(), 60);
  EXPECT_EQ(mgr->encoder(), "auto");
  EXPECT_TRUE(mgr->separateAudioTracks());
  EXPECT_TRUE(mgr->replayCachePath().isEmpty());
  EXPECT_EQ(mgr->getRestoreToken("Deadlock"), "token-abc");
}

TEST_F(SettingsManagerTest, LoadEmitsFullSignalSet) {
  // load() must notify every key (wizard/Settings bindings settle on
  // these); asserting only settingsLoaded let a dropped emit hide.
  QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());
  mgr->setVideoBitrate(4242);
  mgr->setFps(144);
  mgr->setEncoder("x264");
  mgr->setSeparateAudioTracks(true);
  mgr->setMicrophoneDevice("mic-a");
  mgr->setMaxStorageSize(777);
  mgr->save();
  QFile saved(filePath);
  ASSERT_TRUE(saved.open(QIODevice::ReadOnly));
  const QByteArray persisted = saved.readAll();
  saved.close();

  // Setters save immediately; restore the earlier snapshot after mutating.
  mgr->setVideoBitrate(1111);
  mgr->setFps(30);
  mgr->setEncoder("auto");
  mgr->setSeparateAudioTracks(false);
  mgr->setMicrophoneDevice("mic-b");
  mgr->setMaxStorageSize(888);
  ASSERT_TRUE(saved.open(QIODevice::WriteOnly | QIODevice::Truncate));
  ASSERT_EQ(saved.write(persisted), persisted.size());
  saved.close();

  QSignalSpy bitrate(mgr.get(), &SettingsManager::videoBitrateChanged);
  QSignalSpy fps(mgr.get(), &SettingsManager::fpsChanged);
  QSignalSpy enc(mgr.get(), &SettingsManager::encoderChanged);
  QSignalSpy tracks(mgr.get(), &SettingsManager::separateAudioTracksChanged);
  QSignalSpy mic(mgr.get(), &SettingsManager::microphoneDeviceChanged);
  QSignalSpy storage(mgr.get(), &SettingsManager::maxStorageSizeChanged);
  mgr->load();

  EXPECT_EQ(mgr->videoBitrate(), 4242);
  EXPECT_EQ(bitrate.count(), 1);
  EXPECT_EQ(fps.count(), 1);
  EXPECT_EQ(enc.count(), 1);
  EXPECT_EQ(tracks.count(), 1);
  EXPECT_EQ(mic.count(), 1);
  EXPECT_EQ(storage.count(), 1);
}

TEST_F(SettingsManagerTest, SaveEmitsSignal) {
  // Ensure config directory exists
  QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());

  QSignalSpy spy(mgr.get(), &SettingsManager::settingsSaved);
  mgr->save();
  EXPECT_EQ(spy.count(), 1);
}

TEST_F(SettingsManagerTest, LoadEmitsSignal) {
  // Ensure config directory exists and save to create the file
  QString filePath = mgr->settingsFilePath();
  QDir().mkpath(QFileInfo(filePath).path());
  mgr->save();

  QSignalSpy spy(mgr.get(), &SettingsManager::settingsLoaded);
  mgr->load();
  EXPECT_EQ(spy.count(), 1);
}
