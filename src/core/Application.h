/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QApplication>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <functional>
#include <memory>

// Forward declarations
namespace TrophyBackend {
class ApplicationAudioProbeTest;
class UpdateChecker;
class RecordingManager;
class SettingsManager;
class StorageManager;
class RecordingLibrary;
class ActivityFeed;
class WaveformCache;
class FilmstripCache;
class GameDetectionService;
class NotificationService;
class AudioMixer;
class WindowService;
class WaylandWindowMonitor;
class HardwareInfo;
class AudioDeviceEnumerator;
class SystemTrayManager;
class AutostartManager;
class ReplayManager;
class GlobalShortcutManager;
class ControlServer;
} // namespace TrophyBackend

namespace TrophyBackend {

/**
 * @brief Main application class that owns all core services
 */
class Application : public QApplication {
  Q_OBJECT

  // Properties exposed to QML
  Q_PROPERTY(bool isRecording READ isRecording NOTIFY recordingStateChanged)
  Q_PROPERTY(QString currentGameName READ currentGameName NOTIFY currentGameChanged)
  Q_PROPERTY(bool isWayland READ isWayland CONSTANT)
  Q_PROPERTY(bool captureAvailable READ captureAvailable NOTIFY captureAvailabilityChanged)
  Q_PROPERTY(bool captureAvailabilityPending READ captureAvailabilityPending NOTIFY captureAvailabilityChanged)
  Q_PROPERTY(bool canStartRecording READ canStartRecording NOTIFY recordingAvailabilityChanged)
  Q_PROPERTY(QString recordingPhase READ recordingPhase NOTIFY recordingStateChanged)
  Q_PROPERTY(bool trayAvailable READ isTrayAvailable NOTIFY trayAvailabilityChanged)
  Q_PROPERTY(bool suppressNextStorageNotification READ suppressNextStorageNotification WRITE setSuppressNextStorageNotification NOTIFY suppressNextStorageNotificationChanged)
  Q_PROPERTY(bool vulkanLayerRegistered READ vulkanLayerRegistered NOTIFY detectionStatusChanged)
  Q_PROPERTY(bool glShimAvailable READ glShimAvailable NOTIFY detectionStatusChanged)
  Q_PROPERTY(bool glShimEnrolled READ glShimEnrolled NOTIFY detectionStatusChanged)
  Q_PROPERTY(bool glShimActive READ glShimActive NOTIFY detectionStatusChanged)

public:
  explicit Application(int &argc, char **argv);
  ~Application() override;

  /**
   * @brief Initialize all services and load QML
   * @return true on success, false on failure
   */
  bool initialize();

  /**
   * @brief Sets the path to the main QML file for development/live-reload
   */
  void setMainQmlPath(const QString &path) { m_mainQmlPath = path; }

  // Service accessors
  RecordingManager *recordingManager() const { return m_recordingManager.get(); }
  SettingsManager *settingsManager() const { return m_settingsManager.get(); }
  StorageManager *storageManager() const { return m_storageManager.get(); }
  RecordingLibrary *recordingLibrary() const { return m_recordingLibrary.get(); }
  ActivityFeed *activityFeed() const { return m_activityFeed.get(); }
  GameDetectionService *gameDetector() const { return m_gameDetector.get(); }
  NotificationService *notificationService() const { return m_notificationService.get(); }
  HardwareInfo *hardwareInfo() const { return m_hardwareInfo.get(); }

  // Property getters
  bool isRecording() const;
  QString currentGameName() const;
  bool isWayland() const;
  bool captureAvailable() const { return m_captureAvailable; }
  bool captureAvailabilityPending() const { return m_captureAvailabilityPending; }
  bool canStartRecording() const;
  QString recordingPhase() const;

  // Encoder detection (called from QML)
  Q_INVOKABLE bool isEncoderAvailable(const QString &name) const;

  // Detection-component status for the setup wizard (live properties).
  bool vulkanLayerRegistered() const { return m_vulkanLayerRegistered; }
  Q_INVOKABLE bool setVulkanLayerEnabled(bool enabled);
  bool glShimAvailable() const;
  bool glShimEnrolled() const;
  bool glShimActive() const;
  Q_INVOKABLE bool setGlShimEnrolled(bool enrolled);

  bool suppressNextStorageNotification() const;
  void setSuppressNextStorageNotification(bool suppress);

  // System tray (close-to-tray, start minimized, tray quit)
  bool isTrayAvailable() const;
  // Called from QML onClosing: hides to tray when enabled, else false so
  // the default close/confirm flow runs.
  Q_INVOKABLE bool minimizeToTray();
  // Tray "Quit": confirms in the main window when recording, else quits.
  Q_INVOKABLE void requestQuit();
  Q_INVOKABLE void showMainWindow();
  // Instant replay: routes tray/F10/hotkey requests with user feedback.
  Q_INVOKABLE void requestReplay();
  // Track mute that follows the active pipeline (the dashboard meters).
  // The recording and the DVR own different mixers; muting only one
  // would silently do nothing half the time, so both are always set and
  // the readback prefers whichever pipeline is running.
  Q_INVOKABLE void setTrackMuted(int trackIndex, bool muted);
  Q_INVOKABLE bool isTrackMuted(int trackIndex) const;
  // Live level from whichever pipeline is running (same routing).
  Q_INVOKABLE float trackLevel(int trackIndex) const;
  // Clip completion chime (pw-play ships with PipeWire; the wav is
  // extracted from resources to cache on first use). Also wired to a
  // Test button in Settings (preview bypasses the toggle). The
  // recording-stopped chime is the same voice descending (high-low)
  // so stops read as an ending against the clip's ascending pair.
  Q_INVOKABLE void playClipPing(bool preview);
  Q_INVOKABLE void playRecordingStoppedPing(bool preview);
  // Global-shortcut record toggle (mirrors F9, with feedback).
  Q_INVOKABLE void requestToggleRecording();
  // Opens a pre-filled GitHub bug report in the browser. Browsers cannot
  // accept a file attachment via URL, so the issue body embeds the app
  // version plus the tail of trophy.log (see BugReport.h) instead.
  Q_INVOKABLE void reportBug();
  // --minimized CLI flag: behave like startMinimized for this launch.
  void setForceMinimized(bool minimized) { m_forceMinimized = minimized; }

signals:
  void captureAvailabilityChanged();
  void recordingAvailabilityChanged();
  void recordingStateChanged();
  void currentGameChanged();
  void suppressNextStorageNotificationChanged();
  void trayAvailabilityChanged();
  void detectionStatusChanged();
  void fatalError(const QString &message);

private:
  void queryCaptureAvailability();
  void applyCaptureAvailability(quint64 generation, bool available);
  bool m_captureAvailable = false;
  bool m_captureAvailabilityPending = false;
  quint64 m_captureAvailabilityGeneration = 0;
  void initializeServices();
  void registerQmlTypes();
  void exposeServicesToQml();
  void connectSignals();
  // Only successful, finalized outputs are warmed (never active recordings or
  // DVR segments). Reuses the player's cache instances and in-flight jobs.
  void preparePlayerCaches(const QString &filePath);
  friend class ApplicationMediaCacheTest;
  // Registers the Vulkan implicit layer; true when the layer JSON is in
  // place (result cached in m_vulkanLayerRegistered at startup).
  bool ensureVulkanLayerRegistered();
  // Deletes our layer JSON; true when nothing remains registered.
  bool removeVulkanLayerRegistration();

  std::unique_ptr<RecordingManager> m_recordingManager;
  std::unique_ptr<SettingsManager> m_settingsManager;
  std::unique_ptr<StorageManager> m_storageManager;
  std::unique_ptr<RecordingLibrary> m_recordingLibrary;
  std::unique_ptr<ActivityFeed> m_activityFeed;
  std::unique_ptr<GameDetectionService> m_gameDetector;
  std::unique_ptr<NotificationService> m_notificationService;
  std::unique_ptr<HardwareInfo> m_hardwareInfo;
  std::unique_ptr<AudioMixer> m_audioMixer;
  std::unique_ptr<WindowService> m_windowService;
  std::unique_ptr<WaylandWindowMonitor> m_windowMonitor;
  std::unique_ptr<AudioDeviceEnumerator> m_audioDeviceEnumerator;
  std::unique_ptr<SystemTrayManager> m_trayManager;
  std::unique_ptr<AutostartManager> m_autostartManager;
  std::unique_ptr<ReplayManager> m_replayManager;
  std::unique_ptr<GlobalShortcutManager> m_shortcutManager;
  std::unique_ptr<ControlServer> m_controlServer;
  std::unique_ptr<QQmlApplicationEngine> m_engine;
  std::unique_ptr<UpdateChecker> m_updateChecker;
  std::unique_ptr<WaveformCache> m_waveformCache;
  std::unique_ptr<FilmstripCache> m_filmstripCache;
  
  QString m_mainQmlPath; // Path for live-reload
  QString m_pendingAutoRecordGame;
  QString m_pendingDvrGame;
  bool m_captureSelectionForRecording = true;
  QTimer *m_detectionDebounceTimer = nullptr;
  // Window-gated capture: a portal restore references concrete windows, so
  // a token-armed capture waits for the game's window before requesting.
  // Approved games keep waiting on snapshots after the initial timeout;
  // a picker only ever follows explicit user action.
  QTimer *m_windowWaitTimer = nullptr;
  QString m_pendingWindowGame;
  QString m_pendingCaptureKey;
  bool m_pendingWindowIsRecord = false;
  static constexpr int kWindowWaitMs = 30000;
  void startWhenWindowReady(const QString &game, bool isRecord);
  void resumePendingCapture();
  // Audio-settled capture: auto-armed recording/DVR waits for the
  // game's PipeWire node to exist (stable across two polls) before
  // starting, so launch sessions don't open on monitor fallback and
  // swap mid-track. Bounded (~20s): the timeout starts anyway with
  // today's fallback behavior. Manual starts never wait.
  QTimer *m_audioWaitTimer = nullptr;
  QString m_pendingAudioGame;
  bool m_pendingAudioIsRecord = false;
  int m_audioWaitHits = 0;
  int m_audioWaitTicks = 0;
  static constexpr int kAudioWaitMs = 2000;
  static constexpr int kAudioWaitMaxTicks = 10;
  // Probe state: PipeWire lookups run on a worker (each can take seconds),
  // so a slow query never stacks (skipped ticks don't count) and never
  // blocks the GUI. Generation drops deliveries superseded by game-close
  // or a re-arm; timeout ticks count resolved polls only.
  bool m_audioProbeInflight = false;
  quint64 m_nextAudioProbeTicket = 0;
  quint64 m_audioProbeTicket = 0;
  quint64 m_audioWaitGeneration = 0;
  friend class ApplicationAudioProbeTest;
  quint64 claimAudioProbe();
  void applyGameAudioProbeResult(quint64 ticket, quint64 generation,
                                bool present,
                                const std::function<void(bool)> &onResult);
  void startWhenAudioSettled(const QString &game, bool isRecord);
  static bool gameAudioNodePresent(const QString &game, qint64 pid);
  // One async presence probe (worker thread); onResult runs on the GUI
  // thread unless superseded by a generation bump (game-close / re-arm).
  void probeGameAudioAsync(const QString &game, qint64 pid,
                           quint64 generation,
                           const std::function<void(bool)> &onResult);
  // Authorizing capture is explicit user consent: record the game on the
  // whitelist (raw match key when the detector still tracks it) so future
  // launches detect instantly and the Games page stays truthful. Runs on
  // both authorize paths: prompt click and silent restore-token start.
  void authorizeGame(const QString &game);
  // Block from the authorize prompt: veto the game in the Games list
  // (same key resolution as authorizeGame) with no capture. Dismissing
  // the prompt takes neither path — the game stays on Prompt.
  void blockGame(const QString &game);
  // Block veto for a pending game (raw detector key when it still tracks
  // the game, else the pending name). Both forms normalize, so exe-case
  // variants match.
  bool isGameBlocked(const QString &game) const;
  // Prompt gate shared by the debounce fire and the window-wait fallback:
  // blocked games and token-approved games never prompt (approval is
  // token-derived; the silent path already owns approved games).
  bool shouldShowAuthorizePrompt(const QString &game) const;
  bool isCurrentCaptureGame(const QString &game) const;
  QString restoreTokenForGame(const QString &game) const;
  void onWindowWaitTimeout();
  void onAuthorizeDebounceFired();
  friend class ApplicationAuthorizePromptTest;
  bool m_suppressNextStorageNotification = false;
  bool m_vulkanLayerRegistered = false;
  bool m_minimizeNotifyShown = false;
  bool m_forceMinimized = false;
  // Update check: the releases-page URL of the latest release
  // seen, so the "View Release" notice action can open it.
  QString m_latestReleaseUrl;
  QTimer *m_updateCheckTimer = nullptr;
  // First background check after startup (short: launch work
  // is done in well under this, and the request is async).
  static constexpr int kUpdateCheckDelayMs = 10000;
  QObject *qmlRootWindow() const;
  void setMainWindowVisible(bool visible);
  void syncAutostartFile();
  // Shared pw-play helper behind both chimes (resource -> cache file).
  void playChime(const QString &resource, const QString &fileName,
                 bool preview);
};

} // namespace TrophyBackend
