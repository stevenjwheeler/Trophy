/*
 * Trophy - Unit Tests
 * Test suite for SettingsManager persistence and property system
 */

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "settings/SettingsManager.h"

using namespace GameRecorder;

class SettingsManagerTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
    // Redirect config path to temp for test isolation
    QCoreApplication::setOrganizationName("TrophyTest");
    QCoreApplication::setApplicationName("TrophyTestApp");
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new SettingsManagerTestEnvironment);

class SettingsManagerTest : public ::testing::Test {
protected:
  void SetUp() override { mgr = std::make_unique<SettingsManager>(); }

  void TearDown() override {
    // Clean up settings file if created
    QString path = mgr->settingsFilePath();
    QFile::remove(path);
    mgr.reset();
  }

  std::unique_ptr<SettingsManager> mgr;
};

// --- Default Values ---

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
  EXPECT_EQ(mgr->audioBitrate(), 192);
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
