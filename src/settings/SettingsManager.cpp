/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsManager.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace GameRecorder {

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
    qInfo() << "Settings file not found, using defaults";
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

  // Load audio settings
  QJsonObject audio = root["audio"].toObject();
  m_audioMode = audio["audioMode"].toString("game");
  m_gameAudioDevice = audio["gameAudioDevice"].toString("default");
  m_microphoneDevice = audio["microphoneDevice"].toString("default");
  m_voiceChatApp = audio["voiceChatApp"].toString("discord");
  m_separateAudioTracks = audio["separateTracks"].toBool(false);
  m_audioBitrate = audio["audioBitrate"].toInt(192);

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

  qInfo() << "Settings loaded from" << filePath;
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
  root["video"] = video;

  // Audio settings
  QJsonObject audio;
  audio["audioMode"] = m_audioMode;
  audio["gameAudioDevice"] = m_gameAudioDevice;
  audio["microphoneDevice"] = m_microphoneDevice;
  audio["voiceChatApp"] = m_voiceChatApp;
  audio["separateTracks"] = m_separateAudioTracks;
  audio["audioBitrate"] = m_audioBitrate;
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
  root["detection"] = detection;

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

  qInfo() << "Settings saved to" << filePath;
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

  emit audioModeChanged();
  emit gameAudioDeviceChanged();
  emit microphoneDeviceChanged();
  emit voiceChatAppChanged();
  emit separateAudioTracksChanged();
  emit audioBitrateChanged();

  emit recordingsPathChanged();
  emit maxStorageSizeChanged();
  emit autoDeleteOldestChanged();

  emit autoDetectGamesChanged();
  emit autoRecordEnabledChanged();
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

  // Audio
  m_audioMode = "game";
  m_gameAudioDevice = "default";
  m_microphoneDevice = "default";
  m_voiceChatApp = "discord";
  m_separateAudioTracks = false;
  m_audioBitrate = 192;

  // Storage
  m_recordingsPath =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) +
      "/GameRecordings";
  m_maxStorageSize = 50LL * 1024 * 1024 * 1024; // 50 GB
  m_autoDeleteOldest = true;

  // Detection
  m_autoDetectGames = true;
  m_autoRecordEnabled = false;
}

void SettingsManager::migrateSettings(int fromVersion) {
  qInfo() << "Migrating settings from version" << fromVersion << "to"
          << SETTINGS_VERSION;

  // Future migrations go here
  // if (fromVersion < 2) { ... }
}

QStringList SettingsManager::availableEncoders() const {
  QStringList encoders;
  encoders << "auto";

#ifdef HAVE_NVENC
  encoders << "nvenc_h264" << "nvenc_hevc";
#endif

#ifdef HAVE_VAAPI
  encoders << "vaapi_h264" << "vaapi_hevc";
#endif

  encoders << "x264";

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

void SettingsManager::setAudioBitrate(int bitrate) {
  if (m_audioBitrate != bitrate) {
    m_audioBitrate = bitrate;
    save();
    emit audioBitrateChanged();
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

} // namespace GameRecorder
