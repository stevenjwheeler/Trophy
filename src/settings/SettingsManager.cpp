/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsManager.h"
#include "encoding/EncoderFactory.h"
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend {

SettingsManager::SettingsManager(QObject *parent) : QObject(parent) {
  // Set default recordings path
  m_recordingsPath =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
      "/GameRecordings";
}

SettingsManager::~SettingsManager() { save(); }

QString SettingsManager::settingsFilePath() const {
  QString configPath =
      QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  return configPath + "/trophy/settings.json";
}

void SettingsManager::load() {
  QString filePath = settingsFilePath();
  QFile file(filePath);

  if (!file.exists()) {
    qDebug() << "Settings file not found, using defaults";
    loadDefaults();
    save(); // Create the file with defaults
    return;
  }

  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "Failed to open settings file:" << file.errorString();
    loadDefaults();
    return;
  }

  QByteArray data = file.readAll();
  file.close();

  QJsonParseError parseError;
  QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

  if (parseError.error != QJsonParseError::NoError) {
    qWarning() << "Failed to parse settings JSON:" << parseError.errorString();
    loadDefaults();
    return;
  }

  QJsonObject root = doc.object();

  // Check version for migration
  int version = root["version"].toInt(1);
  if (version < SETTINGS_VERSION) {
    migrateSettings(version);
  }

  // Load video settings
  QJsonObject video = root["video"].toObject();
  m_videoBitrate = video["bitrate"].toInt(15000);
  m_videoResolution = video["resolution"].toString("native");
  m_fps = video["fps"].toInt(60);
  m_encoder = video["encoder"].toString("auto");
  m_containerFormat = video["format"].toString("mkv");
  m_encoderPreset = video["preset"].toString("fast");
  m_maxVideoBitrate = video["maxBitrate"].toInt(20000);
  m_useVBR = video["useVBR"].toBool(true);
  m_useDmaBuf = video["useDmaBuf"].toBool(true);
  m_clipBufferSeconds = video["clipBufferSeconds"].toInt(30);

  // Load audio settings
  QJsonObject audio = root["audio"].toObject();
  m_audioMode = audio["audioMode"].toString("game");
  m_gameAudioDevice = audio["gameAudioDevice"].toString("default");
  m_microphoneDevice = audio["microphoneDevice"].toString("default");
  m_voiceChatApp = audio["voiceChatApp"].toString("discord");
  // Separate tracks is the default: per-track isolation means one dead
  // stream (voice churn, missing node) can't stall the whole mix, and the
  // file layout is always game/voice/mic. Matches the header initializer.
  m_separateAudioTracks = audio["separateTracks"].toBool(true);

  // Load storage settings
  QJsonObject storage = root["storage"].toObject();
  QString defaultPath =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
      "/GameRecordings";
  m_recordingsPath = storage["recordingsPath"].toString(defaultPath);
  m_maxStorageSize = static_cast<qint64>(
      storage["maxStorageSizeGB"].toDouble(50.0) * 1024 * 1024 * 1024);
  m_autoDeleteOldest = storage["autoDelete"].toBool(true);

  // Load detection settings
  QJsonObject detection = root["detection"].toObject();
  m_autoDetectGames = detection["autoDetect"].toBool(true);
  m_autoRecordEnabled = detection["autoRecord"].toBool(false);
  m_windowTrackingEnabled = detection["windowTracking"].toBool(false);
  m_vulkanLayerEnabled = detection["vulkanLayer"].toBool(false);
  m_setupCompleted = detection["setupCompleted"].toBool(false);

  // Load general settings
  QJsonObject general = root["general"].toObject();
  m_minimizeToTray = general["minimizeToTray"].toBool(true);
  m_startMinimized = general["startMinimized"].toBool(false);
  m_startOnLogin = general["startOnLogin"].toBool(false);
  m_instantReplayEnabled = general["instantReplay"].toBool(true);
  m_globalShortcutsEnabled = general["globalShortcuts"].toBool(true);
  m_updateCheckEnabled = general["updateCheck"].toBool(true);
  m_replayCachePath = general["replayCachePath"].toString();

  // Load notification options (all default on)
  QJsonObject notifications = root["notifications"].toObject();
  m_notifyRecordings = notifications["recordings"].toBool(true);
  m_notifyClips = notifications["clips"].toBool(true);
  m_notifyStorage = notifications["storage"].toBool(true);
  // Renamed from "clipPing" once the recording-stopped chime joined
  // it; the legacy key is still honoured so existing users keep their
  // preference (first save rewrites it under the new name).
  if (notifications.contains("completionSound"))
    m_completionSoundEnabled = notifications["completionSound"].toBool(true);
  else
    m_completionSoundEnabled = notifications["clipPing"].toBool(true);
  
  // Load Wayland restore tokens (per-game map; a legacy singular
  // "waylandRestoreToken" key is ignored — it was never mapped to a game).
  if (root.contains("waylandRestoreTokens")) {
    m_waylandRestoreTokens = root["waylandRestoreTokens"].toObject().toVariantMap();
  }

  qDebug() << "Settings loaded from" << filePath;
  // load() assigns members directly (never via setters), so no saves fire
  // here — verified: only plain m_ assignments plus the no-op migration
  // hook above. But no change signals were emitted either, so QML bound
  // before load would keep stale values. Emit the full property-change
  // set so bindings refresh (unconditional: cheap, and load is rare).
  // Covers every Q_PROPERTY NOTIFY signal in SettingsManager.h
  // (encoderDisplayName shares encoderChanged; settingsLoaded/Saved are
  // not property signals and are handled separately).
  emit videoBitrateChanged();
  emit videoResolutionChanged();
  emit fpsChanged();
  emit encoderChanged();
  emit containerFormatChanged();
  emit encoderPresetChanged();
  emit maxVideoBitrateChanged();
  emit useVBRChanged();
  emit useDmaBufChanged();
  emit clipBufferSecondsChanged();
  emit autoResolutionChanged();
  emit audioModeChanged();
  emit gameAudioDeviceChanged();
  emit microphoneDeviceChanged();
  emit voiceChatAppChanged();
  emit separateAudioTracksChanged();
  emit recordingsPathChanged();
  emit maxStorageSizeChanged();
  emit autoDeleteOldestChanged();
  emit autoDetectGamesChanged();
  emit autoRecordEnabledChanged();
  emit windowTrackingEnabledChanged();
  emit vulkanLayerEnabledChanged();
  emit setupCompletedChanged();
  emit minimizeToTrayChanged();
  emit startMinimizedChanged();
  emit startOnLoginChanged();
  emit instantReplayEnabledChanged();
  emit globalShortcutsEnabledChanged();
  emit updateCheckEnabledChanged();
  emit notifyRecordingsChanged();
  emit notifyClipsChanged();
  emit notifyStorageChanged();
  emit completionSoundEnabledChanged();
  emit replayCachePathChanged();
  emit waylandRestoreTokensChanged();
  emit settingsLoaded();
}

void SettingsManager::save() {
  QString filePath = settingsFilePath();

  // Ensure directory exists
  QDir dir = QFileInfo(filePath).dir();
  if (!dir.exists()) {
    dir.mkpath(".");
  }

  QJsonObject root;
  root["version"] = SETTINGS_VERSION;

  // Video settings
  QJsonObject video;
  video["bitrate"] = m_videoBitrate;
  video["resolution"] = m_videoResolution;
  video["fps"] = m_fps;
  video["encoder"] = m_encoder;
  video["format"] = m_containerFormat;
  video["preset"] = m_encoderPreset;
  video["maxBitrate"] = m_maxVideoBitrate;
  video["useVBR"] = m_useVBR;
  video["useDmaBuf"] = m_useDmaBuf;
  video["clipBufferSeconds"] = m_clipBufferSeconds;
  root["video"] = video;

  // Audio settings
  QJsonObject audio;
  audio["audioMode"] = m_audioMode;
  audio["gameAudioDevice"] = m_gameAudioDevice;
  audio["microphoneDevice"] = m_microphoneDevice;
  audio["voiceChatApp"] = m_voiceChatApp;
  audio["separateTracks"] = m_separateAudioTracks;
  root["audio"] = audio;

  // Storage settings
  QJsonObject storage;
  storage["recordingsPath"] = m_recordingsPath;
  storage["maxStorageSizeGB"] =
      static_cast<double>(m_maxStorageSize) / (1024.0 * 1024.0 * 1024.0);
  storage["autoDelete"] = m_autoDeleteOldest;
  root["storage"] = storage;

  // Detection settings
  QJsonObject detection;
  detection["autoDetect"] = m_autoDetectGames;
  detection["autoRecord"] = m_autoRecordEnabled;
  detection["windowTracking"] = m_windowTrackingEnabled;
  detection["vulkanLayer"] = m_vulkanLayerEnabled;
  detection["setupCompleted"] = m_setupCompleted;
  root["detection"] = detection;

  // General settings
  QJsonObject general;
  general["minimizeToTray"] = m_minimizeToTray;
  general["startMinimized"] = m_startMinimized;
  general["startOnLogin"] = m_startOnLogin;
  general["instantReplay"] = m_instantReplayEnabled;
  general["globalShortcuts"] = m_globalShortcutsEnabled;
  general["updateCheck"] = m_updateCheckEnabled;
  general["replayCachePath"] = m_replayCachePath;
  root["general"] = general;

  // Notification options
  QJsonObject notifications;
  notifications["recordings"] = m_notifyRecordings;
  notifications["clips"] = m_notifyClips;
  notifications["storage"] = m_notifyStorage;
  notifications["completionSound"] = m_completionSoundEnabled;
  root["notifications"] = notifications;

  root["waylandRestoreTokens"] = QJsonObject::fromVariantMap(m_waylandRestoreTokens);

  // Write atomically using QSaveFile
  QSaveFile file(filePath);
  if (!file.open(QIODevice::WriteOnly)) {
    qWarning() << "Failed to open settings file for writing:"
               << file.errorString();
    return;
  }

  QJsonDocument doc(root);
  file.write(doc.toJson(QJsonDocument::Indented));

  if (!file.commit()) {
    qWarning() << "Failed to save settings:" << file.errorString();
    return;
  }

  qDebug() << "Settings saved to" << filePath;
  emit settingsSaved();
}

void SettingsManager::resetToDefaults() {
  loadDefaults();
  save();

  // Emit all change signals so the UI updates
  emit videoBitrateChanged();
  emit videoResolutionChanged();
  emit fpsChanged();
  emit encoderChanged();
  emit containerFormatChanged();
  emit encoderPresetChanged();
  emit maxVideoBitrateChanged();
  emit useVBRChanged();
  emit useDmaBufChanged();
  emit clipBufferSecondsChanged();
  emit autoResolutionChanged();

  emit audioModeChanged();
  emit gameAudioDeviceChanged();
  emit microphoneDeviceChanged();
  emit voiceChatAppChanged();
  emit separateAudioTracksChanged();

  emit recordingsPathChanged();
  emit maxStorageSizeChanged();
  emit autoDeleteOldestChanged();

  emit autoDetectGamesChanged();
  emit autoRecordEnabledChanged();
  emit windowTrackingEnabledChanged();
  emit vulkanLayerEnabledChanged();
  emit setupCompletedChanged();
  emit minimizeToTrayChanged();
  emit startMinimizedChanged();
  emit startOnLoginChanged();
  emit instantReplayEnabledChanged();
  emit globalShortcutsEnabledChanged();
  emit updateCheckEnabledChanged();
  emit replayCachePathChanged();
  emit notifyRecordingsChanged();
  emit notifyClipsChanged();
  emit notifyStorageChanged();
  emit completionSoundEnabledChanged();
  emit waylandRestoreTokensChanged();
}

void SettingsManager::loadDefaults() {
  // Video
  m_videoBitrate = 15000;
  m_videoResolution = "native";
  m_fps = 60;
  m_encoder = "auto";
  m_containerFormat = "mkv";
  m_encoderPreset = "fast";
  m_maxVideoBitrate = 20000;
  m_useVBR = true;
  m_useDmaBuf = true;
  m_clipBufferSeconds = 30;

  // Audio
  m_audioMode = "game";
  m_gameAudioDevice = "default";
  m_microphoneDevice = "default";
  m_voiceChatApp = "discord";
  m_separateAudioTracks = true;

  // Storage
  m_recordingsPath =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
      "/GameRecordings";
  m_maxStorageSize = 50LL * 1024 * 1024 * 1024; // 50 GB
  m_autoDeleteOldest = true;

  // Detection
  m_autoDetectGames = true;
  m_autoRecordEnabled = false;
  m_windowTrackingEnabled = false;
  m_vulkanLayerEnabled = false;

  // System tray
  m_minimizeToTray = true;
  m_startMinimized = false;
  m_startOnLogin = false;
  m_instantReplayEnabled = true;
  m_globalShortcutsEnabled = true;
  m_updateCheckEnabled = true;
  // DVR segment dir: empty means the /tmp default. Reset like every other
  // persisted value.
  m_replayCachePath.clear();
  // NOTE: m_waylandRestoreTokens deliberately untouched — per-game portal
  // authorizations survive a settings reset.

  // Notifications (all on)
  m_notifyRecordings = true;
  m_notifyClips = true;
  m_notifyStorage = true;
  m_completionSoundEnabled = true;
}

void SettingsManager::migrateSettings(int fromVersion) {
  qDebug() << "Migrating settings from version" << fromVersion << "to"
          << SETTINGS_VERSION;

  // Future migrations go here
  // if (fromVersion < 2) { ... }
}

QStringList SettingsManager::availableEncoders() const {
  QStringList encoders;
  encoders << "auto";

  auto detected = EncoderFactory::detectAvailableEncoders();
  for (const auto &enc : detected) {
    encoders << enc.name;
  }

  return encoders;
}

// Property setters with change notification and auto-save
void SettingsManager::setVideoBitrate(int bitrate) {
  if (m_videoBitrate != bitrate) {
    m_videoBitrate = bitrate;
    save();
    emit videoBitrateChanged();
  }
}

void SettingsManager::setVideoResolution(const QString &resolution) {
  if (m_videoResolution != resolution) {
    m_videoResolution = resolution;
    save();
    emit videoResolutionChanged();
  }
}

void SettingsManager::setFps(int fps) {
  if (m_fps != fps) {
    m_fps = fps;
    save();
    emit fpsChanged();
  }
}

void SettingsManager::setEncoder(const QString &encoder) {
  if (m_encoder != encoder) {
    m_encoder = encoder;
    save();
    emit encoderChanged();
  }
}

QString SettingsManager::encoderDisplayName() const {
  if (m_encoder == "auto") {
    return tr("Automatic");
  }

  auto encoders = EncoderFactory::detectAvailableEncoders();
  for (const auto &enc : encoders) {
    if (enc.name == m_encoder) {
      return enc.description;
    }
  }

  return m_encoder;
}

void SettingsManager::setContainerFormat(const QString &format) {
  if (m_containerFormat != format) {
    m_containerFormat = format;
    save();
    emit containerFormatChanged();
  }
}

void SettingsManager::setEncoderPreset(const QString &preset) {
  if (m_encoderPreset != preset) {
    m_encoderPreset = preset;
    save();
    emit encoderPresetChanged();
  }
}

void SettingsManager::setAudioMode(const QString &mode) {
  if (m_audioMode != mode) {
    m_audioMode = mode;
    save();
    emit audioModeChanged();
  }
}

void SettingsManager::setGameAudioDevice(const QString &device) {
  if (m_gameAudioDevice != device) {
    m_gameAudioDevice = device;
    save();
    emit gameAudioDeviceChanged();
  }
}

void SettingsManager::setMicrophoneDevice(const QString &device) {
  if (m_microphoneDevice != device) {
    m_microphoneDevice = device;
    save();
    emit microphoneDeviceChanged();
  }
}

void SettingsManager::setVoiceChatApp(const QString &app) {
  if (m_voiceChatApp != app) {
    m_voiceChatApp = app;
    save();
    emit voiceChatAppChanged();
  }
}

void SettingsManager::setSeparateAudioTracks(bool separate) {
  if (m_separateAudioTracks != separate) {
    m_separateAudioTracks = separate;
    save();
    emit separateAudioTracksChanged();
  }
}

void SettingsManager::setRecordingsPath(const QString &path) {
  if (m_recordingsPath != path) {
    m_recordingsPath = path;
    save();
    emit recordingsPathChanged();
  }
}

void SettingsManager::setMaxStorageSize(qint64 bytes) {
  if (m_maxStorageSize != bytes) {
    m_maxStorageSize = bytes;
    save();
    emit maxStorageSizeChanged();
  }
}

void SettingsManager::setAutoDeleteOldest(bool enabled) {
  if (m_autoDeleteOldest != enabled) {
    m_autoDeleteOldest = enabled;
    save();
    emit autoDeleteOldestChanged();
  }
}

void SettingsManager::setAutoDetectGames(bool enabled) {
  if (m_autoDetectGames != enabled) {
    m_autoDetectGames = enabled;
    save();
    emit autoDetectGamesChanged();
  }
}

void SettingsManager::setAutoRecordEnabled(bool enabled) {
  if (m_autoRecordEnabled != enabled) {
    m_autoRecordEnabled = enabled;
    save();
    emit autoRecordEnabledChanged();
  }
}

void SettingsManager::setWindowTrackingEnabled(bool enabled) {
  if (m_windowTrackingEnabled != enabled) {
    m_windowTrackingEnabled = enabled;
    save();
    emit windowTrackingEnabledChanged();
  }
}

void SettingsManager::setVulkanLayerEnabled(bool enabled) {
  if (m_vulkanLayerEnabled != enabled) {
    m_vulkanLayerEnabled = enabled;
    save();
    emit vulkanLayerEnabledChanged();
  }
}

void SettingsManager::setMinimizeToTray(bool enabled) {
  if (m_minimizeToTray != enabled) {
    m_minimizeToTray = enabled;
    save();
    emit minimizeToTrayChanged();
  }
}

void SettingsManager::setStartMinimized(bool enabled) {
  if (m_startMinimized != enabled) {
    m_startMinimized = enabled;
    save();
    emit startMinimizedChanged();
  }
}

void SettingsManager::setStartOnLogin(bool enabled) {
  if (m_startOnLogin != enabled) {
    m_startOnLogin = enabled;
    save();
    emit startOnLoginChanged();
  }
}

void SettingsManager::setInstantReplayEnabled(bool enabled) {
  if (m_instantReplayEnabled != enabled) {
    m_instantReplayEnabled = enabled;
    save();
    emit instantReplayEnabledChanged();
  }
}

void SettingsManager::setGlobalShortcutsEnabled(bool enabled) {
  if (m_globalShortcutsEnabled != enabled) {
    m_globalShortcutsEnabled = enabled;
    save();
    emit globalShortcutsEnabledChanged();
  }
}

void SettingsManager::setUpdateCheckEnabled(bool enabled) {
  if (m_updateCheckEnabled != enabled) {
    m_updateCheckEnabled = enabled;
    save();
    emit updateCheckEnabledChanged();
  }
}
void SettingsManager::setNotifyRecordings(bool enabled) {
  if (m_notifyRecordings != enabled) {
    m_notifyRecordings = enabled;
    save();
    emit notifyRecordingsChanged();
  }
}

void SettingsManager::setNotifyClips(bool enabled) {
  if (m_notifyClips != enabled) {
    m_notifyClips = enabled;
    save();
    emit notifyClipsChanged();
  }
}

void SettingsManager::setNotifyStorage(bool enabled) {
  if (m_notifyStorage != enabled) {
    m_notifyStorage = enabled;
    save();
    emit notifyStorageChanged();
  }
}

void SettingsManager::setCompletionSoundEnabled(bool enabled) {
  if (m_completionSoundEnabled != enabled) {
    m_completionSoundEnabled = enabled;
    save();
    emit completionSoundEnabledChanged();
  }
}

void SettingsManager::setReplayCachePath(const QString &path) {
  if (m_replayCachePath != path) {
    m_replayCachePath = path;
    save();
    emit replayCachePathChanged();
  }
}

void SettingsManager::setMaxVideoBitrate(int bitrate) {
  if (m_maxVideoBitrate != bitrate) {
    m_maxVideoBitrate = bitrate;
    save();
    emit maxVideoBitrateChanged();
  }
}

void SettingsManager::setUseVBR(bool enabled) {
  if (m_useVBR != enabled) {
    m_useVBR = enabled;
    save();
    emit useVBRChanged();
  }
}

void SettingsManager::setUseDmaBuf(bool enabled) {
  if (m_useDmaBuf != enabled) {
    m_useDmaBuf = enabled;
    save();
    emit useDmaBufChanged();
  }
}

void SettingsManager::setClipBufferSeconds(int seconds) {
  if (m_clipBufferSeconds != seconds) {
    m_clipBufferSeconds = seconds;
    save();
    emit clipBufferSecondsChanged();
  }
}

QString SettingsManager::getRestoreToken(const QString &key) const {
  return m_waylandRestoreTokens.value(key).toString();
}

void SettingsManager::setRestoreToken(const QString &key, const QString &token) {
  if (m_waylandRestoreTokens.value(key).toString() != token) {
    m_waylandRestoreTokens[key] = token;
    save();
    emit waylandRestoreTokensChanged();
  }
}

void SettingsManager::removeRestoreToken(const QString &key) {
  if (m_waylandRestoreTokens.contains(key)) {
    m_waylandRestoreTokens.remove(key);
    save();
    emit waylandRestoreTokensChanged();
  }
}

void SettingsManager::setSetupCompleted(bool completed) {
  if (m_setupCompleted != completed) {
    m_setupCompleted = completed;
    save();
    emit setupCompletedChanged();
  }
}

void SettingsManager::setAutoResolutionWidth(int width) {
  if (m_autoResolutionWidth != width) {
    m_autoResolutionWidth = width;
    emit autoResolutionChanged();
  }
}

void SettingsManager::setAutoResolutionHeight(int height) {
  if (m_autoResolutionHeight != height) {
    m_autoResolutionHeight = height;
    emit autoResolutionChanged();
  }
}

} // namespace TrophyBackend
