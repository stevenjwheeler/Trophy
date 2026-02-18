/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace GameRecorder {

/**
 * @brief Manages application settings with JSON persistence
 *
 * Settings are stored in ~/.config/trophy/settings.json
 * and include video, audio, storage, and detection configuration.
 */
class SettingsManager : public QObject {
  Q_OBJECT

  // Video settings
  Q_PROPERTY(int videoBitrate READ videoBitrate WRITE setVideoBitrate NOTIFY
                 videoBitrateChanged)
  Q_PROPERTY(QString videoResolution READ videoResolution WRITE
                 setVideoResolution NOTIFY videoResolutionChanged)
  Q_PROPERTY(int fps READ fps WRITE setFps NOTIFY fpsChanged)
  Q_PROPERTY(
      QString encoder READ encoder WRITE setEncoder NOTIFY encoderChanged)
  Q_PROPERTY(QString containerFormat READ containerFormat WRITE
                 setContainerFormat NOTIFY containerFormatChanged)
  Q_PROPERTY(QString encoderPreset READ encoderPreset WRITE setEncoderPreset
                 NOTIFY encoderPresetChanged)
  Q_PROPERTY(int maxVideoBitrate READ maxVideoBitrate WRITE setMaxVideoBitrate
                 NOTIFY maxVideoBitrateChanged)
  Q_PROPERTY(bool useVBR READ useVBR WRITE setUseVBR NOTIFY useVBRChanged)

  // Audio settings
  Q_PROPERTY(QString audioMode READ audioMode WRITE setAudioMode NOTIFY
                 audioModeChanged)
  Q_PROPERTY(QString gameAudioDevice READ gameAudioDevice WRITE
                 setGameAudioDevice NOTIFY gameAudioDeviceChanged)
  Q_PROPERTY(QString microphoneDevice READ microphoneDevice WRITE
                 setMicrophoneDevice NOTIFY microphoneDeviceChanged)
  Q_PROPERTY(QString voiceChatApp READ voiceChatApp WRITE setVoiceChatApp NOTIFY
                 voiceChatAppChanged)
  Q_PROPERTY(bool separateAudioTracks READ separateAudioTracks WRITE
                 setSeparateAudioTracks NOTIFY separateAudioTracksChanged)
  Q_PROPERTY(int audioBitrate READ audioBitrate WRITE setAudioBitrate NOTIFY
                 audioBitrateChanged)

  // Storage settings
  Q_PROPERTY(QString recordingsPath READ recordingsPath WRITE setRecordingsPath
                 NOTIFY recordingsPathChanged)
  Q_PROPERTY(qint64 maxStorageSize READ maxStorageSize WRITE setMaxStorageSize
                 NOTIFY maxStorageSizeChanged)
  Q_PROPERTY(bool autoDeleteOldest READ autoDeleteOldest WRITE
                 setAutoDeleteOldest NOTIFY autoDeleteOldestChanged)

  // Detection settings
  Q_PROPERTY(bool autoDetectGames READ autoDetectGames WRITE setAutoDetectGames
                 NOTIFY autoDetectGamesChanged)
  Q_PROPERTY(bool autoRecordEnabled READ autoRecordEnabled WRITE
                 setAutoRecordEnabled NOTIFY autoRecordEnabledChanged)

public:
  explicit SettingsManager(QObject *parent = nullptr);
  ~SettingsManager() override;

  // Persistence
  Q_INVOKABLE void load();
  Q_INVOKABLE void save();
  Q_INVOKABLE void resetToDefaults();

  // Video settings
  int videoBitrate() const { return m_videoBitrate; }
  void setVideoBitrate(int bitrate);

  QString videoResolution() const { return m_videoResolution; }
  void setVideoResolution(const QString &resolution);

  int maxVideoBitrate() const { return m_maxVideoBitrate; }
  void setMaxVideoBitrate(int bitrate);

  bool useVBR() const { return m_useVBR; }
  void setUseVBR(bool enabled);

  QString resolution() const { return m_videoResolution; }

  int fps() const { return m_fps; }
  void setFps(int fps);

  QString encoder() const { return m_encoder; }
  void setEncoder(const QString &encoder);

  QString containerFormat() const { return m_containerFormat; }
  void setContainerFormat(const QString &format);

  QString encoderPreset() const { return m_encoderPreset; }
  void setEncoderPreset(const QString &preset);

  // Audio settings
  QString audioMode() const { return m_audioMode; }
  void setAudioMode(const QString &mode);

  QString gameAudioDevice() const { return m_gameAudioDevice; }
  void setGameAudioDevice(const QString &device);

  QString microphoneDevice() const { return m_microphoneDevice; }
  void setMicrophoneDevice(const QString &device);

  QString voiceChatApp() const { return m_voiceChatApp; }
  void setVoiceChatApp(const QString &app);

  bool separateAudioTracks() const { return m_separateAudioTracks; }
  void setSeparateAudioTracks(bool separate);

  int audioBitrate() const { return m_audioBitrate; }
  void setAudioBitrate(int bitrate);

  // Storage settings
  QString recordingsPath() const { return m_recordingsPath; }
  void setRecordingsPath(const QString &path);

  qint64 maxStorageSize() const { return m_maxStorageSize; }
  void setMaxStorageSize(qint64 bytes);

  bool autoDeleteOldest() const { return m_autoDeleteOldest; }
  void setAutoDeleteOldest(bool enabled);

  // Detection settings
  bool autoDetectGames() const { return m_autoDetectGames; }
  void setAutoDetectGames(bool enabled);

  bool autoRecordEnabled() const { return m_autoRecordEnabled; }
  void setAutoRecordEnabled(bool enabled);

  // Utility
  Q_INVOKABLE QString settingsFilePath() const;
  Q_INVOKABLE QStringList availableEncoders() const;

signals:
  // Video
  void videoBitrateChanged();
  void videoResolutionChanged();
  void fpsChanged();
  void encoderChanged();
  void containerFormatChanged();
  void encoderPresetChanged();
  void maxVideoBitrateChanged();
  void useVBRChanged();

  // Audio
  void audioModeChanged();
  void gameAudioDeviceChanged();
  void microphoneDeviceChanged();
  void voiceChatAppChanged();
  void separateAudioTracksChanged();
  void audioBitrateChanged();

  // Storage
  void recordingsPathChanged();
  void maxStorageSizeChanged();
  void autoDeleteOldestChanged();

  // Detection
  void autoDetectGamesChanged();
  void autoRecordEnabledChanged();

  // General
  void settingsLoaded();
  void settingsSaved();

private:
  void loadDefaults();
  void migrateSettings(int fromVersion);

  // Settings file version for migrations
  static constexpr int SETTINGS_VERSION = 1;

  // Video settings
  int m_videoBitrate = 15000; // kbps
  QString m_videoResolution = "native";
  int m_fps = 60;
  QString m_encoder = "auto";        // auto, nvenc, vaapi, x264
  QString m_containerFormat = "mkv"; // mkv, mp4
  QString m_encoderPreset = "fast";
  int m_maxVideoBitrate = 20000;
  bool m_useVBR = true;

  // Audio settings
  QString m_audioMode = "game";
  QString m_gameAudioDevice = "default";
  QString m_microphoneDevice = "default";
  QString m_voiceChatApp = "discord";
  bool m_separateAudioTracks = true;
  int m_audioBitrate = 192; // kbps

  // Storage settings
  QString m_recordingsPath;
  qint64 m_maxStorageSize = 50LL * 1024 * 1024 * 1024; // 50 GB
  bool m_autoDeleteOldest = true;

  // Detection settings
  bool m_autoDetectGames = true;
  bool m_autoRecordEnabled = false;
};

} // namespace GameRecorder
