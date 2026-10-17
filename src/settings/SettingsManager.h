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

namespace TrophyBackend {

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
  Q_PROPERTY(QString encoderDisplayName READ encoderDisplayName NOTIFY encoderChanged)
  Q_PROPERTY(QString containerFormat READ containerFormat WRITE
                 setContainerFormat NOTIFY containerFormatChanged)
  Q_PROPERTY(QString encoderPreset READ encoderPreset WRITE setEncoderPreset
                 NOTIFY encoderPresetChanged)
  Q_PROPERTY(int maxVideoBitrate READ maxVideoBitrate WRITE setMaxVideoBitrate
                 NOTIFY maxVideoBitrateChanged)
  Q_PROPERTY(bool useVBR READ useVBR WRITE setUseVBR NOTIFY useVBRChanged)
  Q_PROPERTY(bool useDmaBuf READ useDmaBuf WRITE setUseDmaBuf NOTIFY
                 useDmaBufChanged)
  Q_PROPERTY(int clipBufferSeconds READ clipBufferSeconds WRITE setClipBufferSeconds NOTIFY clipBufferSecondsChanged)
  Q_PROPERTY(QString resolution READ resolution NOTIFY videoResolutionChanged)
  Q_PROPERTY(int autoResolutionWidth READ autoResolutionWidth NOTIFY autoResolutionChanged)
  Q_PROPERTY(int autoResolutionHeight READ autoResolutionHeight NOTIFY autoResolutionChanged)

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

  // Storage settings
  Q_PROPERTY(QString recordingsPath READ recordingsPath WRITE setRecordingsPath
                 NOTIFY recordingsPathChanged)
  // DVR segment dir; empty means the /tmp default (see ReplayManager).
  // Was a plain getter until 2026-10-01, so QML reads got `undefined`
  // (startup "Unable to assign [undefined]" warning) and writes from the
  // Settings Browse/Default buttons landed on a dynamic property instead
  // of the C++ member — the whole field was dead.
  Q_PROPERTY(QString replayCachePath READ replayCachePath WRITE
                 setReplayCachePath NOTIFY replayCachePathChanged)
  Q_PROPERTY(qint64 maxStorageSize READ maxStorageSize WRITE setMaxStorageSize
                 NOTIFY maxStorageSizeChanged)
  Q_PROPERTY(bool autoDeleteOldest READ autoDeleteOldest WRITE
                 setAutoDeleteOldest NOTIFY autoDeleteOldestChanged)

  // Detection settings
  Q_PROPERTY(bool autoDetectGames READ autoDetectGames WRITE setAutoDetectGames
                 NOTIFY autoDetectGamesChanged)
  Q_PROPERTY(bool autoRecordEnabled READ autoRecordEnabled WRITE
                 setAutoRecordEnabled NOTIFY autoRecordEnabledChanged)
  Q_PROPERTY(bool windowTrackingEnabled READ windowTrackingEnabled WRITE
                 setWindowTrackingEnabled NOTIFY windowTrackingEnabledChanged)
  Q_PROPERTY(bool vulkanLayerEnabled READ vulkanLayerEnabled WRITE
                 setVulkanLayerEnabled NOTIFY vulkanLayerEnabledChanged)
  Q_PROPERTY(bool setupCompleted READ setupCompleted WRITE setSetupCompleted
                 NOTIFY setupCompletedChanged)
  Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray
                 NOTIFY minimizeToTrayChanged)
  Q_PROPERTY(bool startMinimized READ startMinimized WRITE setStartMinimized
                 NOTIFY startMinimizedChanged)
  Q_PROPERTY(bool startOnLogin READ startOnLogin WRITE setStartOnLogin
                 NOTIFY startOnLoginChanged)
  Q_PROPERTY(bool instantReplayEnabled READ instantReplayEnabled WRITE
                 setInstantReplayEnabled NOTIFY instantReplayEnabledChanged)
  Q_PROPERTY(bool globalShortcutsEnabled READ globalShortcutsEnabled WRITE
                 setGlobalShortcutsEnabled NOTIFY globalShortcutsEnabledChanged)
  // Daily update check against the GitHub release feed (the
  // notice points at the releases page; phase 1 does not
  // download anything). Manual checks from the Info page run
  // regardless of this toggle.
  Q_PROPERTY(bool updateCheckEnabled READ updateCheckEnabled WRITE
                 setUpdateCheckEnabled NOTIFY updateCheckEnabledChanged)

  // Notification options (which ambient notices may appear; errors, the
  // DVR authorize prompt and direct keypress feedback always show).
  Q_PROPERTY(bool notifyRecordings READ notifyRecordings WRITE
                 setNotifyRecordings NOTIFY notifyRecordingsChanged)
  Q_PROPERTY(bool notifyClips READ notifyClips WRITE setNotifyClips NOTIFY
                 notifyClipsChanged)
  Q_PROPERTY(bool notifyStorage READ notifyStorage WRITE setNotifyStorage
                 NOTIFY notifyStorageChanged)
  // Short chimes when a clip or a recording finishes saving (played
  // via pw-play, which ships with PipeWire itself — no extra backend
  // needed).
  Q_PROPERTY(bool completionSoundEnabled READ completionSoundEnabled WRITE setCompletionSoundEnabled
                 NOTIFY completionSoundEnabledChanged)
  // Dashboard library layout: false = grid, true = list. Persisted so the
  // dashboard reopens in the user's last-used mode.
  Q_PROPERTY(bool libraryListView READ libraryListView WRITE setLibraryListView
                  NOTIFY libraryListViewChanged)
  Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
  Q_PROPERTY(QString accentColor READ accentColor WRITE setAccentColor NOTIFY accentColorChanged)
  
  // Wayland Restore Tokens
  Q_PROPERTY(QVariantMap waylandRestoreTokens READ waylandRestoreTokens NOTIFY waylandRestoreTokensChanged)

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

  bool useDmaBuf() const { return m_useDmaBuf; }
  void setUseDmaBuf(bool enabled);

  int clipBufferSeconds() const { return m_clipBufferSeconds; }
  void setClipBufferSeconds(int seconds);

  QString resolution() const { return m_videoResolution; }

  int fps() const { return m_fps; }
  void setFps(int fps);

  int autoResolutionWidth() const { return m_autoResolutionWidth; }
  void setAutoResolutionWidth(int width);
  int autoResolutionHeight() const { return m_autoResolutionHeight; }
  void setAutoResolutionHeight(int height);

  QString encoder() const { return m_encoder; }
  void setEncoder(const QString &encoder);
  QString encoderDisplayName() const;

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

  bool windowTrackingEnabled() const { return m_windowTrackingEnabled; }
  void setWindowTrackingEnabled(bool enabled);

  bool vulkanLayerEnabled() const { return m_vulkanLayerEnabled; }
  void setVulkanLayerEnabled(bool enabled);

  bool setupCompleted() const { return m_setupCompleted; }
  void setSetupCompleted(bool completed);

  bool minimizeToTray() const { return m_minimizeToTray; }
  void setMinimizeToTray(bool enabled);

  bool startMinimized() const { return m_startMinimized; }
  void setStartMinimized(bool enabled);

  bool startOnLogin() const { return m_startOnLogin; }
  void setStartOnLogin(bool enabled);

  bool instantReplayEnabled() const { return m_instantReplayEnabled; }
  void setInstantReplayEnabled(bool enabled);

  bool globalShortcutsEnabled() const { return m_globalShortcutsEnabled; }
  void setGlobalShortcutsEnabled(bool enabled);

  bool updateCheckEnabled() const { return m_updateCheckEnabled; }
  void setUpdateCheckEnabled(bool enabled);

  // Ambient-notice toggles (errors/authorize/keypress feedback always show).
  bool notifyRecordings() const { return m_notifyRecordings; }
  void setNotifyRecordings(bool enabled);

  bool notifyClips() const { return m_notifyClips; }
  void setNotifyClips(bool enabled);

  bool notifyStorage() const { return m_notifyStorage; }
  void setNotifyStorage(bool enabled);

  bool completionSoundEnabled() const { return m_completionSoundEnabled; }
  void setCompletionSoundEnabled(bool enabled);

  // Dashboard library layout (false = grid, true = list).
  bool libraryListView() const { return m_libraryListView; }
  void setLibraryListView(bool enabled);

  QString themeMode() const { return m_themeMode; }
  void setThemeMode(const QString &mode);
  QString accentColor() const { return m_accentColor; }
  void setAccentColor(const QString &color);

  // Replay cache dir for DVR segments; empty = /tmp/trophy/replay (RAM-backed).
  // Point it at /tmp (tmpfs) to hold the rolling buffer in RAM.
  QString replayCachePath() const { return m_replayCachePath; }
  void setReplayCachePath(const QString &path);

  QVariantMap waylandRestoreTokens() const { return m_waylandRestoreTokens; }
  Q_INVOKABLE QString getRestoreToken(const QString &key) const;
  QString getRestoreTokenForIdentity(const QString &key, const QString &legacyName) const;
  void setRestoreTokenForIdentity(const QString &key, const QString &legacyName,
                                 const QString &token);
  Q_INVOKABLE void setRestoreToken(const QString &key, const QString &token);
  Q_INVOKABLE void removeRestoreToken(const QString &key);

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
  void useDmaBufChanged();
  void clipBufferSecondsChanged();
  void autoResolutionChanged();

  // Audio
  void audioModeChanged();
  void gameAudioDeviceChanged();
  void microphoneDeviceChanged();
  void voiceChatAppChanged();
  void separateAudioTracksChanged();

  // Storage
  void recordingsPathChanged();
  void maxStorageSizeChanged();
  void autoDeleteOldestChanged();

  // Detection
  void autoDetectGamesChanged();
  void autoRecordEnabledChanged();
  void windowTrackingEnabledChanged();
  void vulkanLayerEnabledChanged();
  void setupCompletedChanged();
  void minimizeToTrayChanged();
  void startMinimizedChanged();
  void startOnLoginChanged();
  void instantReplayEnabledChanged();
  void globalShortcutsEnabledChanged();
  void updateCheckEnabledChanged();
  void notifyRecordingsChanged();
  void notifyClipsChanged();
  void notifyStorageChanged();
  void completionSoundEnabledChanged();
  void libraryListViewChanged();
  void themeModeChanged();
  void accentColorChanged();
  void replayCachePathChanged();
  void waylandRestoreTokensChanged();

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
  bool m_useDmaBuf = true;
  int m_clipBufferSeconds = 30;
  int m_autoResolutionWidth = 0;
  int m_autoResolutionHeight = 0;

  // Audio settings
  QString m_audioMode = "game";
  QString m_gameAudioDevice = "default";
  QString m_microphoneDevice = "default";
  QString m_voiceChatApp = "discord";
  bool m_separateAudioTracks = true;

  // Storage settings
  QString m_recordingsPath;
  qint64 m_maxStorageSize = 50LL * 1024 * 1024 * 1024; // 50 GB
  bool m_autoDeleteOldest = true;

  // Detection settings
  bool m_autoDetectGames = true;
  bool m_autoRecordEnabled = false;
  // Compositor window tracking (KWin script etc.). Opt-in via the setup
  // wizard; nothing is installed until the user enables it.
  bool m_windowTrackingEnabled = false;
  // Vulkan detection layer. Opt-in via the setup wizard; never
  // auto-registered.
  bool m_vulkanLayerEnabled = false;
  bool m_setupCompleted = false;

  // System tray behaviour
  bool m_minimizeToTray = true;
  bool m_startMinimized = false;
  bool m_startOnLogin = false;
  // Instant replay DVR (rolling segments while a game runs)
  bool m_instantReplayEnabled = true;
  // Portal global shortcuts (F9/F10); bound on user consent, never silently.
  bool m_globalShortcutsEnabled = true;
  // Daily GitHub release check (default on; Info page manual
  // check always runs).
  bool m_updateCheckEnabled = true;
  QString m_replayCachePath;
  // Ambient-notice toggles (all default on; errors, the DVR authorize
  // prompt and direct keypress feedback always show regardless).
  bool m_notifyRecordings = true;
  bool m_notifyClips = true;
  bool m_notifyStorage = true;
  bool m_completionSoundEnabled = true;
  // Dashboard library layout (false = grid, true = list).
  bool m_libraryListView = false;
  QString m_themeMode = "dark";
  QString m_accentColor = "rose";
  
  // Wayland Restore Tokens
  QVariantMap m_waylandRestoreTokens;
};

} // namespace TrophyBackend
