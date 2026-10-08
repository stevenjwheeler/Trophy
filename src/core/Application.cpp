/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <qdebug.h>
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

#include <unistd.h> // isatty(STDIN_FILENO) for launch-context logging

#include "Application.h"
#include "AutostartManager.h"
#include "ControlServer.h"
#include "GlobalShortcutManager.h"
#include "NotificationService.h"
#include "ReplayManager.h"
#include "HardwareInfo.h"
#include "RecordingManager.h"
#include "StorageManager.h"
#include "SystemTrayManager.h"
#include "WindowService.h"
#include "audio/AudioDeviceEnumerator.h"
#include "audio/AudioMixer.h"
#include "audio/PipeWireAudioSource.h"
#include "capture/CaptureFactory.h"
#include "detection/GameDetectionService.h"
#include "detection/GlShimEnrollment.h"
#include "detection/VulkanLayer.h"
#include "detection/WaylandWindowMonitor.h"
#include "encoding/EncoderFactory.h"
#include "library/ActivityFeed.h"
#include "library/RecordingLibrary.h"
#include "settings/SettingsManager.h"
#include "UpdateChecker.h"

#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QProcess>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <cstdio>

Q_LOGGING_CATEGORY(appLog, "trophy.app")

namespace TrophyBackend {

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv) {}

Application::~Application() = default;

bool Application::initialize() {
  // 1. Force Qt to enable debug logging for our categories
  // This ensures messages actually reach our handler
  QLoggingCategory::setFilterRules("*.debug=true\n"
                                   "qt.*.debug=false\n"
                                   "qml.debug=false");

  QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/trophy";
  QDir().mkpath(configDir);
  QString logPath = configDir + "/trophy.log";

  // Open log file in append mode
  static FILE* logFile = std::fopen(logPath.toUtf8().constData(), "a");

  qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    const char *level = "UNKNOWN";
    bool isImportant = (type != QtDebugMsg);

    switch (type) {
        case QtDebugMsg:    level = "DEBUG";    break;
        case QtInfoMsg:     level = "INFO";     break;
        case QtWarningMsg:  level = "WARNING";  break;
        case QtCriticalMsg: level = "CRITICAL"; break;
        case QtFatalMsg:    level = "FATAL";    break;
    }

    // --- FILTERS ---
    QString category = QString::fromLatin1(context.category);
    QString filePath = QString::fromLatin1(context.file ? context.file : "");
    
    // Identify if the message comes from YOUR code.
    // If it's in the src directory, or uses the "default" category, we treat it as Trophy.
    bool isTrophySource = filePath.contains("/src/") || 
                          filePath.contains("src/") || 
                          (category == "default");
    
    // Identify "Noise" (Qt internals, QML engine babble)
    bool isTechnicalNoise = category.startsWith("qt.") || 
                           category.startsWith("qml") || 
                           category.startsWith("dbus.");

    // --- 1. LOG FILE LOGIC ---
    if (logFile && (isTrophySource || isImportant)) {
        QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
        std::fprintf(logFile, "[%s] [%s] [%s] %s\n", 
                     timestamp.toUtf8().constData(), 
                     level,
                     context.category,
                     msg.toUtf8().constData());
        std::fflush(logFile);
    }

    // --- 2. CONSOLE LOGIC ---
    // Usage: TROPHY_DEBUG=1 trophy
    static bool forceDebug = qEnvironmentVariableIsSet("TROPHY_DEBUG");
    
    if (isImportant || forceDebug) {
        // Keep console clean: only show noise if it's actually an error
        if (!isTechnicalNoise || isImportant) {
            std::fprintf(stderr, "[%s] %s\n", level, msg.toUtf8().constData());
            std::fflush(stderr);
        }
    }
  });

#ifdef APP_VERSION
  QString versionStr = QString::fromUtf8(TOSTRING(APP_VERSION));
  versionStr.replace("\"", "");
#else
  QString versionStr = "0.0.0-unknown";
#endif

  setApplicationVersion(versionStr);
  qCInfo(appLog) << "Initializing Trophy - Game recording and clipping" << versionStr;

  initializeServices();

  // Claim the control socket early (before QML/tray/detection): remote
  // --toggle-record/--save-clip/--show-window commands sent during startup
  // would otherwise find nobody listening and be dropped. Normally main()'s
  // instance lock guarantees no rival; a false return here means we lost a
  // concurrent-startup race, so forward a raise and refuse to run doubled.
  if (m_controlServer && !m_controlServer->listen()) {
    ControlServer::sendCommand(QStringLiteral("show-window")); // best effort
    qCCritical(appLog) << "Another Trophy instance is running; exiting.";
    return false;
  }
  // The Vulkan layer is optional: only (re)register when enabled. A user
  // who opted out keeps no layer JSON behind.
  if (m_settingsManager->vulkanLayerEnabled()) {
    m_vulkanLayerRegistered = ensureVulkanLayerRegistered();
  } else {
    removeVulkanLayerRegistration();
    m_vulkanLayerRegistered = false;
  }
  emit detectionStatusChanged();
  registerQmlTypes();

  m_engine = std::make_unique<QQmlApplicationEngine>();
  m_engine->rootContext()->setContextProperty("AppVersion", versionStr);

  exposeServicesToQml();
  connectSignals();

  // --- PREPARE PROPERTIES FOR REQUIRED TAGS IN QML ---
  QVariantMap initialProperties;
  initialProperties.insert("app", QVariant::fromValue(this));
  initialProperties.insert("recordingManager", QVariant::fromValue(m_recordingManager.get()));
  initialProperties.insert("settingsManager", QVariant::fromValue(m_settingsManager.get()));
  initialProperties.insert("storageManager", QVariant::fromValue(m_storageManager.get()));
  initialProperties.insert("recordingLibrary", QVariant::fromValue(m_recordingLibrary.get()));
  initialProperties.insert("gameDetector", QVariant::fromValue(m_gameDetector.get()));
  initialProperties.insert("updateChecker", QVariant::fromValue(m_updateChecker.get()));

  m_engine->setInitialProperties(initialProperties);

  QUrl url;
  if (!m_mainQmlPath.isEmpty()) {
      url = QUrl::fromLocalFile(m_mainQmlPath);
      qCInfo(appLog) << "Live Reload active: Loading QML from filesystem:" << url;
  } else {
      url = QUrl(QStringLiteral("qrc:/qt/qml/TrophyBackend/qml/main.qml"));
  }

  QObject::connect(
      m_engine.get(), &QQmlApplicationEngine::objectCreationFailed, this,
      [this, url]() {
        qCCritical(appLog) << "Failed to load QML:" << url;
        emit fatalError("Failed to load user interface");
        QCoreApplication::exit(1);
      },
      Qt::QueuedConnection);

  m_engine->load(url);

  if (m_engine->rootObjects().isEmpty()) {
    qCCritical(appLog) << "No root objects created from QML";
    return false;
  }

  if (m_trayManager && m_trayManager->isAvailable()) {
    m_trayManager->setTrayVisible(true);
    m_trayManager->setRecording(m_recordingManager->isRecording(),
                                 m_recordingManager->currentGameName());
    emit trayAvailabilityChanged();
    if (m_settingsManager->startMinimized() || m_forceMinimized) {
      qCInfo(appLog) << "Starting minimized to system tray";
      setMainWindowVisible(false);
    }
  } else {
    qCInfo(appLog) << "No system tray host; running with plain window";
  }

  syncAutostartFile();

  // Global shortcuts register once the event loop spins (portal D-Bus) —
  // but only after first-run setup: the wizard's Shortcuts step asks
  // first, so the portal approval dialog never ambushes a new user.
  QTimer::singleShot(0, this, [this]() {
    if (m_shortcutManager && m_settingsManager &&
        m_settingsManager->setupCompleted() &&
        m_settingsManager->globalShortcutsEnabled())
      m_shortcutManager->requestShortcuts();
  });

  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::X11) {
    qCCritical(appLog) << "Trophy does not support native X11 sessions.";
    emit fatalError("Trophy does not support native X11 sessions. Please switch to a Wayland session.");
    return false;
  }

  if (CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland) {
    if (!CaptureFactory::isPipeWireAvailable()) {
      qCCritical(appLog) << "PipeWire or ScreenCast portal not found!";
      emit fatalError("PipeWire or ScreenCast portal not found. Screen capture is unavailable.");
      return false;
    }
  }

  m_gameDetector->start();
  // Launch context: portal restore tokens are attributed per app identity,
  // so a token saved by a terminal run may not restore in a launcher run
  // (and vice versa). GIO_LAUNCHED_DESKTOP_FILE only exists for gio/GNOME
  // launches, so stdin being a tty is the more reliable terminal signal
  // on KDE — log the raw facts plus the guess.
  const bool stdinTty = ::isatty(STDIN_FILENO);
  const QByteArray gioEnv = qgetenv("GIO_LAUNCHED_DESKTOP_FILE");
  qCInfo(appLog) << "Launched from:" << QCoreApplication::applicationFilePath()
                 << "stdin-tty:" << stdinTty << "desktop-file-env:"
                 << (gioEnv.isEmpty() ? QStringLiteral("none")
                                     : QString::fromLocal8Bit(gioEnv))
                 << (stdinTty ? "(likely terminal)" : "(likely launcher)");
  qCInfo(appLog) << "Application initialized successfully";
  return true;
}

void Application::initializeServices() {
  m_settingsManager = std::make_unique<SettingsManager>(this);
  m_settingsManager->load();
  m_storageManager = std::make_unique<StorageManager>(m_settingsManager->recordingsPath(), this);
  m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
  m_storageManager->setAutoDeleteOldest(m_settingsManager->autoDeleteOldest());
  m_notificationService = std::make_unique<NotificationService>(this);
  m_hardwareInfo = std::make_unique<HardwareInfo>(this);
  m_recordingLibrary = std::make_unique<RecordingLibrary>(m_settingsManager->recordingsPath(), this);
  m_activityFeed = std::make_unique<ActivityFeed>(this);
  m_audioMixer = std::make_unique<AudioMixer>(48000, 2, this);
  m_audioDeviceEnumerator = std::make_unique<AudioDeviceEnumerator>(this);
  m_gameDetector = std::make_unique<GameDetectionService>(this);
  m_windowService = std::make_unique<WindowService>(this);
  m_windowMonitor = std::make_unique<WaylandWindowMonitor>(this);
  m_windowService->setWindowMonitor(m_windowMonitor.get());
  m_gameDetector->setWindowMonitor(m_windowMonitor.get());
  if (m_settingsManager->windowTrackingEnabled())
    m_windowMonitor->startMonitoring();
  else
    // Heal orphans: a script left behind by an older build, a deleted
    // settings file or a crash mid-toggle is removed so a disabled
    // switch never reports "Installed". Mirrors the Vulkan cleanup in
    // initialize().
    m_windowMonitor->uninstallBackend();
  connect(m_settingsManager.get(),
          &SettingsManager::windowTrackingEnabledChanged, this, [this]() {
            if (m_settingsManager->windowTrackingEnabled()) {
              m_windowMonitor->startMonitoring();
            } else {
              // Explicit opt-out: stop tracking and remove the script
              // package we installed (reinstalled on re-enable).
              m_windowMonitor->uninstallBackend();
            }
          });
  m_recordingManager = std::make_unique<RecordingManager>(this);
  
  m_recordingManager->setSettingsManager(m_settingsManager.get());
  m_recordingManager->setStorageManager(m_storageManager.get());
  m_recordingManager->setAudioMixer(m_audioMixer.get());
  m_recordingManager->setWindowService(m_windowService.get());
  m_recordingManager->setGameDetectionService(m_gameDetector.get());

  m_detectionDebounceTimer = new QTimer(this);
  m_detectionDebounceTimer->setSingleShot(true);
  m_detectionDebounceTimer->setInterval(3000);

  m_windowWaitTimer = new QTimer(this);
  m_windowWaitTimer->setSingleShot(true);
  m_windowWaitTimer->setInterval(kWindowWaitMs);

  m_audioWaitTimer = new QTimer(this);
  m_audioWaitTimer->setSingleShot(false);
  m_audioWaitTimer->setInterval(kAudioWaitMs);

  m_trayManager = std::make_unique<SystemTrayManager>(this);
  m_autostartManager = std::make_unique<AutostartManager>(this);
  m_replayManager = std::make_unique<ReplayManager>(this);
  m_replayManager->setSettingsManager(m_settingsManager.get());
  m_shortcutManager = std::make_unique<GlobalShortcutManager>(this);
  m_shortcutManager->setEnabled(m_settingsManager->globalShortcutsEnabled());
  m_controlServer = std::make_unique<ControlServer>(this);
  connect(m_controlServer.get(), &ControlServer::toggleRecordingRequested,
          this, [this]() { requestToggleRecording(); });
  connect(m_controlServer.get(), &ControlServer::saveClipRequested, this,
          [this]() { requestReplay(); });
  connect(m_controlServer.get(), &ControlServer::showWindowRequested, this,
          [this]() { showMainWindow(); });

  // Update check: compares against the GitHub release feed.
  // applicationVersion() carries APP_VERSION (the CI tag on
  // release builds). Branch stamps never compare against a
  // release tag — parseVersion rejects them, so a dev build
  // stays silent instead of reading as "behind".
  m_updateChecker = std::make_unique<UpdateChecker>(this);
  m_updateChecker->setCurrentVersion(
      QCoreApplication::applicationVersion());
  connect(m_updateChecker.get(), &UpdateChecker::checkFinished, this,
          [this](bool newerAvailable, const QString &latestVersion,
                 const QString &releaseUrl) {
            m_latestReleaseUrl = releaseUrl;
            if (!newerAvailable ||
                !m_settingsManager->updateCheckEnabled())
              return;
            // Normal urgency + a single "View Release" button.
            // Deliberately not critical: an update never needs
            // to shout over a fullscreen game, and the Info
            // page carries the same status for anyone who
            // misses the transient.
            m_notificationService->showNotification(
                QStringLiteral("Trophy Update Available"),
                QStringLiteral("Trophy %1 is available — you're on %2.")
                    .arg(latestVersion,
                         QCoreApplication::applicationVersion()),
                QStringLiteral("trophy"), 8000, 1, false,
                QStringLiteral("open-update"),
                QStringLiteral("View Release"));
          });
  // Daily cadence while running: first check ~60s after
  // startup (never racing launch), then every 24h. The app
  // can't check while closed, so "daily" means once per day
  // the app is actually up.
  m_updateCheckTimer = new QTimer(this);
  m_updateCheckTimer->setSingleShot(false);
  m_updateCheckTimer->setInterval(24 * 60 * 60 * 1000);
  connect(m_updateCheckTimer, &QTimer::timeout, this, [this]() {
    if (m_settingsManager->updateCheckEnabled())
      m_updateChecker->checkNow();
  });
  QTimer::singleShot(60000, this, [this]() {
    if (m_settingsManager->updateCheckEnabled())
      m_updateChecker->checkNow();
    m_updateCheckTimer->start();
  });
}

void Application::registerQmlTypes() {
  qRegisterMetaType<AudioDeviceInfo>("AudioDeviceInfo");
}

void Application::exposeServicesToQml() {
  QQmlContext *context = m_engine->rootContext();
  context->setContextProperty("app", this);
  context->setContextProperty("recordingManager", m_recordingManager.get());
  context->setContextProperty("settingsManager", m_settingsManager.get());
  context->setContextProperty("storageManager", m_storageManager.get());
  context->setContextProperty("recordingLibrary", m_recordingLibrary.get());
  context->setContextProperty("activityFeed", m_activityFeed.get());
  context->setContextProperty("gameDetector", m_gameDetector.get());
  context->setContextProperty("notificationService", m_notificationService.get());
  context->setContextProperty("hardwareInfo", m_hardwareInfo.get());
  context->setContextProperty("audioMixer", m_audioMixer.get());
  context->setContextProperty("windowService", m_windowService.get());
  context->setContextProperty("windowMonitor", m_windowMonitor.get());
  context->setContextProperty("audioDeviceEnumerator", m_audioDeviceEnumerator.get());
  context->setContextProperty("globalShortcuts", m_shortcutManager.get());
  context->setContextProperty("replayManager", m_replayManager.get());
  context->setContextProperty("updateChecker", m_updateChecker.get());
}

void Application::connectSignals() {
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this, &Application::recordingStateChanged);
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this, [this](const QString &filePath) {
            m_recordingLibrary->setActiveRecordingPath(filePath);
            // The background replay loop pauses while recording.
            m_replayManager->setPaused(true);
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this, [this](const QString &filePath, qint64, qint64) {
            m_recordingLibrary->setActiveRecordingPath("");
            m_recordingLibrary->refresh();
            // Resume the replay loop if the game is still running.
            m_replayManager->setPaused(false);
            if (m_gameDetector && !m_gameDetector->currentGameName().isEmpty()) {
              m_replayManager->startDvr(m_gameDetector->currentGameName());
            }
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameDetected, this, [this](const QString &gameName, qint64 pid) {
            qCInfo(appLog) << "Game detected:" << gameName;
            emit currentGameChanged();
            if (m_settingsManager->autoRecordEnabled()) {
              QString token = m_settingsManager->getRestoreToken(gameName);
              if (!token.isEmpty()) {
                m_detectionDebounceTimer->stop();
                m_pendingAutoRecordGame.clear();
                authorizeGame(gameName);
                startWhenWindowReady(gameName, true);
              } else if (m_pendingAutoRecordGame != gameName ||
                         !m_detectionDebounceTimer->isActive()) {
                m_pendingAutoRecordGame = gameName;
                m_detectionDebounceTimer->start();
              }
            } else if (m_settingsManager->instantReplayEnabled()) {
              // Silent when a restore token exists (no portal popup);
              // otherwise arm through the same authorize-click channel
              // as recording — never a surprise picker at launch.
              if (!m_settingsManager->getRestoreToken(gameName).isEmpty()) {
                m_pendingDvrGame.clear();
                authorizeGame(gameName);
                startWhenWindowReady(gameName, false);
              } else if (m_pendingDvrGame != gameName ||
                         !m_detectionDebounceTimer->isActive()) {
                m_pendingDvrGame = gameName;
                m_detectionDebounceTimer->start();
              }
            }
          });
  // Approval is derived from portal restore tokens: whenever the token
  // map changes, push the holding games to the detector so the Games
  // page's Ready section stays truthful.
  connect(m_settingsManager.get(), &SettingsManager::waylandRestoreTokensChanged,
          this, [this]() {
            if (m_gameDetector && m_settingsManager) {
              m_gameDetector->setTokenGames(
                  m_settingsManager->waylandRestoreTokens().keys());
            }
          });
  if (m_gameDetector && m_settingsManager) {
    m_gameDetector->setTokenGames(
        m_settingsManager->waylandRestoreTokens().keys());
  }
  connect(m_windowMonitor.get(), &WaylandWindowMonitor::windowsChanged, this, [this]() {
            if (m_pendingWindowGame.isEmpty() ||
                !m_gameDetector->hasVisibleWindow())
              return;
            const QString game = m_pendingWindowGame;
            const bool isRecord = m_pendingWindowIsRecord;
            m_pendingWindowGame.clear();
            m_windowWaitTimer->stop();
            qCDebug(appLog) << "Window appeared for" << game
                            << "- waiting for settled game audio";
            startWhenAudioSettled(game, isRecord);
          });
  connect(m_windowWaitTimer, &QTimer::timeout, this, [this]() {
            if (m_pendingWindowGame.isEmpty())
              return;
            // No window in time: fall back to the authorize prompt, never a
            // bare picker. The click then starts capture as the user's own
            // action (a picker there has context).
            const QString game = m_pendingWindowGame;
            const bool isRecord = m_pendingWindowIsRecord;
            m_pendingWindowGame.clear();
            qCDebug(appLog) << "No window appeared for" << game
                            << "- falling back to authorize prompt";
            if (isRecord)
              m_pendingAutoRecordGame = game;
            else
              m_pendingDvrGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_detectionDebounceTimer, &QTimer::timeout, this, [this]() {
    const QString pending = !m_pendingAutoRecordGame.isEmpty()
                                ? m_pendingAutoRecordGame
                                : m_pendingDvrGame;
    if (pending.isEmpty())
      return;
    // A game blocked while detection was pending never prompts: drop it
    // (unblocking re-arms below, so the row's next state always applies).
    if (isGameBlocked(pending)) {
      qCDebug(appLog) << "Skipping authorize prompt for blocked game:"
                      << pending;
      if (m_pendingAutoRecordGame == pending)
        m_pendingAutoRecordGame.clear();
      if (m_pendingDvrGame == pending)
        m_pendingDvrGame.clear();
      return;
    }
    m_notificationService->showNotification("New Game: " + pending, "Click to authorize Wayland capture for this game, or Block it.", "trophy", 0, 2, true);
  });
  connect(m_audioWaitTimer, &QTimer::timeout, this, [this]() {
    if (m_pendingAudioGame.isEmpty()) {
      m_audioWaitTimer->stop();
      return;
    }
    const QString game = m_pendingAudioGame;
    const bool isRecord = m_pendingAudioIsRecord;
    qint64 pid = 0;
    if (m_gameDetector && m_gameDetector->currentGameName() == game)
      pid = m_gameDetector->currentGamePid();
    ++m_audioWaitTicks;
    if (gameAudioNodePresent(game, pid)) {
      if (++m_audioWaitHits >= 2) {
        qCDebug(appLog) << "Game audio settled for" << game << "- starting";
        m_pendingAudioGame.clear();
        m_audioWaitTimer->stop();
        if (isRecord)
          m_recordingManager->startRecording(game);
        else
          m_replayManager->startDvr(game);
        return;
      }
    } else {
      m_audioWaitHits = 0;
    }
    if (m_audioWaitTicks >= kAudioWaitMaxTicks) {
      qCDebug(appLog) << "Game audio never appeared for" << game
                      << "- starting anyway (monitor fallback)";
      m_pendingAudioGame.clear();
      m_audioWaitTimer->stop();
      if (isRecord)
        m_recordingManager->startRecording(game);
      else
        m_replayManager->startDvr(game);
    }
  });
  connect(m_recordingManager.get(), &RecordingManager::restoreAuthNeeded, this, [this](const QString &game) {
            // Stale token: delete it so the next attempt can't reuse it,
            // then authorize again through the prompt. The picker that may
            // follow the click is the user's own action, never a surprise.
            qCDebug(appLog) << "Restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_pendingAutoRecordGame = game;
            m_detectionDebounceTimer->start();
          });
  connect(m_replayManager.get(), &ReplayManager::restoreAuthNeeded, this, [this](const QString &game) {
            qCDebug(appLog) << "DVR restore token stale for" << game
                            << "- re-authorizing via prompt";
            m_settingsManager->removeRestoreToken(game);
            m_pendingDvrGame = game;
            m_detectionDebounceTimer->start();
          });
  // Unblocking the live game re-arms its prompt: the debounce dropped it
  // as blocked, and same-game re-detects never refire. Tokened games
  // start silently, so only the no-token prompt paths re-arm.
  connect(m_gameDetector.get(), &GameDetectionService::blacklistChanged, this, [this]() {
            if (!m_gameDetector || m_gameDetector->currentGameName().isEmpty())
              return;
            const QString game = m_gameDetector->currentGameName();
            if (isGameBlocked(game) || !m_pendingAutoRecordGame.isEmpty() ||
                !m_pendingDvrGame.isEmpty() ||
                !m_settingsManager->getRestoreToken(game).isEmpty())
              return;
            if (m_settingsManager->autoRecordEnabled())
              m_pendingAutoRecordGame = game;
            else if (m_settingsManager->instantReplayEnabled())
              m_pendingDvrGame = game;
            else
              return;
            m_detectionDebounceTimer->start();
          });
  connect(m_gameDetector.get(), &GameDetectionService::gameClosed, this, [this](const QString &gameName) {
            emit currentGameChanged();
            // A resident authorize prompt for the closing game must not
            // linger: it would show a dead name, and clicks on it hit
            // cleared pendings (vanish, nothing starts).
            if (m_pendingAutoRecordGame == gameName ||
                m_pendingDvrGame == gameName) {
              m_notificationService->closeAuthorizePrompt(gameName);
            }
            if (m_pendingAutoRecordGame == gameName) {
              m_pendingAutoRecordGame.clear();
            }
            if (m_pendingDvrGame == gameName) {
              m_pendingDvrGame.clear();
            }
            if (m_pendingWindowGame == gameName) {
              m_pendingWindowGame.clear();
              m_windowWaitTimer->stop();
            }
            if (m_pendingAudioGame == gameName) {
              m_pendingAudioGame.clear();
              m_audioWaitTimer->stop();
            }
            if (m_pendingAutoRecordGame.isEmpty() && m_pendingDvrGame.isEmpty()) {
              m_detectionDebounceTimer->stop();
            }
            m_replayManager->stopDvr();
            if (m_recordingManager->isRecording() && m_recordingManager->currentGameName() == gameName) {
              m_recordingManager->stopRecording();
            }
          });
  connect(m_settingsManager.get(), &SettingsManager::recordingsPathChanged, this, [this]() {
        m_storageManager->setStoragePath(m_settingsManager->recordingsPath());
        m_recordingLibrary->setLibraryPath(m_settingsManager->recordingsPath());
      });
  connect(m_settingsManager.get(), &SettingsManager::maxStorageSizeChanged, this, [this]() {
            m_storageManager->setMaxSize(m_settingsManager->maxStorageSize());
          });
  connect(m_settingsManager.get(), &SettingsManager::autoDeleteOldestChanged,
          this, [this]() {
            m_storageManager->setAutoDeleteOldest(
                m_settingsManager->autoDeleteOldest());
          });
  // Live audio retargeting: device/app picks apply to whichever pipeline
  // is running (full recording wins over DVR). Without a session the
  // setters no-op and the next build picks the new values up.
  connect(m_settingsManager.get(), &SettingsManager::microphoneDeviceChanged,
          this, [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyMicrophoneDevice();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyMicrophoneDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::gameAudioDeviceChanged,
          this, [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyGameAudioDevice();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyGameAudioDevice();
          });
  connect(m_settingsManager.get(), &SettingsManager::voiceChatAppChanged, this,
          [this]() {
            if (m_recordingManager && m_recordingManager->isRecording())
              m_recordingManager->applyVoiceChatApp();
            else if (m_replayManager && m_replayManager->isActive())
              m_replayManager->applyVoiceChatApp();
          });
  // Layout changes (mode/tracks) can't hot-swap: the DVR rebuilds at the
  // next segment rotation, recordings at the next start. Say so when the
  // change lands mid-DVR, or the new layout seemingly "does nothing" for
  // up to a rotation. (During recording the UI locks these outright.)
  // The copy states the rotation bound concretely, from the constant.
  const QString segmentBound = QStringLiteral("Takes effect within %1 seconds.")
                                   .arg(ReplayManager::kSegmentSeconds);
  connect(m_settingsManager.get(), &SettingsManager::audioModeChanged, this,
          [this, segmentBound]() {
            if (m_replayManager && m_replayManager->isActive())
              // Critical like the other transient notices: the user is
              // fullscreen gaming, where normal urgency never surfaces.
              m_notificationService->showNotification(
                  "Audio Mode Changed", segmentBound, "trophy", 3000, 2);
          });
  connect(m_settingsManager.get(), &SettingsManager::separateAudioTracksChanged,
          this, [this, segmentBound]() {
            if (m_replayManager && m_replayManager->isActive())
              m_notificationService->showNotification(
                  "Track Layout Changed", segmentBound, "trophy", 3000, 2);
          });
  connect(m_storageManager.get(), &StorageManager::storageLimitReached, this,
          [this]() {
            if (m_suppressNextStorageNotification) {
              m_suppressNextStorageNotification = false;
              return;
            }
            if (!m_settingsManager->notifyStorage())
              return;
            if (m_settingsManager->autoDeleteOldest()) {
              m_notificationService->showNotification(
                  "Storage Full",
                  "Recordings exceed the storage limit. Oldest files will be auto-deleted, or free space manually.",
                  "dialog-warning", 0, 2);
            } else {
              m_notificationService->showNotification(
                  "Storage Full",
                  "Recordings exceed the storage limit. Enable auto-cleanup or delete old recordings manually.",
                  "dialog-warning", 0, 2);
            }
          });
  connect(m_storageManager.get(), &StorageManager::storageLimitCannotCleanup,
          m_recordingManager.get(), &RecordingManager::stopRecording);
  connect(m_storageManager.get(), &StorageManager::storageLimitCannotCleanup,
          this, [this]() {
            m_notificationService->showNotification(
                "Recording Stopped",
                "Recording stopped: recording exceeds maximum storage size.",
                "dialog-warning", 0, 2);
          });
  connect(m_storageManager.get(), &StorageManager::recordingDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecording);
  // Bulk-delete chain: the worker unlinks in the background, the batch
  // signal settles the model once, and finish unsuppresses the watcher
  // with one catch-up refresh. While suppressed, per-file watcher
  // refreshes on both sides are skipped (the storm that froze the UI
  // on multi-select deletes: N size rescans + N model rebuilds).
  connect(m_storageManager.get(), &StorageManager::recordingsDeleted, m_recordingLibrary.get(), &RecordingLibrary::removeRecordings);
  connect(m_storageManager.get(), &StorageManager::bulkDeleteStarted, m_recordingLibrary.get(), [this]() {
    m_recordingLibrary->setBulkOperationActive(true);
  });
  connect(m_storageManager.get(), &StorageManager::bulkDeleteFinished, m_recordingLibrary.get(), [this](int, int) {
    m_recordingLibrary->setBulkOperationActive(false);
    m_recordingLibrary->refresh();
  });
  // Order matters: the rename preference must be recorded before the
  // refresh recreates Recording objects (which consult it in addRecording).
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::onRecordingRenamed);
  connect(m_storageManager.get(), &StorageManager::recordingRenamed, m_recordingLibrary.get(), &RecordingLibrary::refresh);
  connect(m_notificationService.get(), &NotificationService::notificationClicked, this, [this](const QString &action) {
            // Update notice: open the releases page the
            // check reported. Handled before the authorize
            // dismissal below so it never clears pending
            // state (the two notices never coexist).
            if (action == QStringLiteral("open-update")) {
              if (!m_latestReleaseUrl.isEmpty())
                QDesktopServices::openUrl(QUrl(m_latestReleaseUrl));
              m_notificationService->closeLastNotification();
              return;
            }
            // Three-way authorize prompt: "authorize" starts capture and
            // allow-lists the game (portal token follows at start);
            // "block" vetoes it in the Games list with no capture;
            // anything else is a dismissal — drop pending, change nothing,
            // start nothing. The game stays on Prompt and re-prompts on
            // its next launch.
            if (action != QStringLiteral("authorize") && action != QStringLiteral("block")) {
              m_pendingAutoRecordGame.clear();
              m_pendingDvrGame.clear();
              m_detectionDebounceTimer->stop();
              return;
            }
            QString game;
            bool isRecord = true;
            if (!m_pendingAutoRecordGame.isEmpty()) {
              game = m_pendingAutoRecordGame;
              m_pendingAutoRecordGame.clear();
            } else if (!m_pendingDvrGame.isEmpty()) {
              game = m_pendingDvrGame;
              isRecord = false;
              m_pendingDvrGame.clear();
            }
            m_notificationService->closeLastNotification();
            if (game.isEmpty())
              return;
            if (action == QStringLiteral("block")) {
              blockGame(game);
              return;
            }
            if (isRecord)
              m_recordingManager->startRecording(game);
            else
              m_replayManager->startDvr(game);
            // Prompt click authorizes capture: allow-list the game.
            authorizeGame(game);
          });
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingAdded, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingRemoved, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingLibrary.get(), &RecordingLibrary::recordingsRemoved, m_storageManager.get(), &StorageManager::refresh);
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this,
          [this](qint64, qint64) { m_storageManager->enforceStorageLimit(); });

  // --- System tray ---
  connect(m_recordingManager.get(), &RecordingManager::stateChanged, this,
          [this]() {
            if (m_trayManager) {
              m_trayManager->setRecording(
                  m_recordingManager->isRecording(),
                  m_recordingManager->currentGameName());
            }
          });
  connect(m_recordingManager.get(), &RecordingManager::progressUpdated, this,
          [this](qint64 duration, qint64 size) {
            if (m_trayManager) {
              m_trayManager->setProgress(duration, size);
            }
          });
  // These were never connected — recording start/stop produced no notice
  // at all (critical urgency alone can't show a notification nobody sends).
  connect(m_recordingManager.get(), &RecordingManager::recordingStarted, this,
          [this](const QString &) {
            if (m_settingsManager->notifyRecordings())
              m_notificationService->showRecordingStarted(
                  m_recordingManager->currentGameName());
          });
  connect(m_recordingManager.get(), &RecordingManager::recordingStopped, this,
          [this](const QString &, qint64 durationMs, qint64 sizeBytes) {
            playRecordingStoppedPing(false);
            if (!m_settingsManager->notifyRecordings())
              return;
            const QString summary = QStringLiteral("%1 · %2").arg(
                NotificationService::formatDuration(durationMs),
                StorageManager::formatSize(sizeBytes));
            m_notificationService->showRecordingStopped(summary);
          });
  connect(m_trayManager.get(), &SystemTrayManager::showWindowRequested, this,
          [this]() { showMainWindow(); });
  connect(m_trayManager.get(), &SystemTrayManager::hideWindowRequested, this,
          [this]() { setMainWindowVisible(false); });
  connect(m_trayManager.get(), &SystemTrayManager::startRecordingRequested,
          this, [this]() {
            if (m_recordingManager->isRecording()) {
              return;
            }
            const QString game = m_gameDetector
                                     ? m_gameDetector->currentGameName()
                                     : QString();
            m_recordingManager->startRecording(game);
          });
  connect(m_trayManager.get(), &SystemTrayManager::stopRecordingRequested,
          this, [this]() {
            if (m_recordingManager->isRecording()) {
              m_recordingManager->stopRecording();
            }
          });
  connect(m_trayManager.get(), &SystemTrayManager::openRecordingsRequested,
          this, [this]() {
            QDesktopServices::openUrl(
                QUrl::fromLocalFile(m_settingsManager->recordingsPath()));
          });
  connect(m_trayManager.get(), &SystemTrayManager::quitRequested, this,
          [this]() { requestQuit(); });
  connect(m_trayManager.get(), &SystemTrayManager::saveReplayRequested, this,
          [this]() { requestReplay(); });
  connect(m_shortcutManager.get(),
          &GlobalShortcutManager::toggleRecordingRequested, this,
          [this]() { requestToggleRecording(); });
  connect(m_shortcutManager.get(), &GlobalShortcutManager::saveClipRequested,
          this, [this]() { requestReplay(); });

  // --- Clips ---
  connect(m_replayManager.get(), &ReplayManager::replayStateChanged, this,
          [this]() {
            if (!m_replayManager->isActive())
              return;
            if (!m_settingsManager->notifyClips())
              return;
            const QString game = m_replayManager->currentGame();
            const int secs =
                std::max(1, m_settingsManager->clipBufferSeconds());
            // Queued: waits out "Recording Saved"'s full display window,
            // then appears stacked alongside (never replacing it).
            m_notificationService->showQueuedNotification(
                game.isEmpty() ? QStringLiteral("Ready to clip")
                               : QStringLiteral("Ready to clip %1").arg(game),
                QStringLiteral("Buffering the last %1 seconds.").arg(secs),
                "trophy", 3000, 2); // critical: visible over fullscreen game
          });
  connect(m_replayManager.get(), &ReplayManager::replaySaved, this,
          [this](const QString &filePath, double seconds) {
            playClipPing(false);
            if (m_settingsManager->notifyClips()) {
              const QString game = m_replayManager->currentGame();
              const qint64 secs = std::max<qint64>(1, llround(seconds));
              const QString span =
                  (secs == 1) ? QStringLiteral("Last 1 second")
                              : QStringLiteral("Last %1 seconds").arg(secs);
              const QString size =
                  StorageManager::formatSize(QFileInfo(filePath).size());
              m_notificationService->showNotification(
                  game.isEmpty() ? QStringLiteral("Clip saved")
                                 : QStringLiteral("%1 clip saved").arg(game),
                  QStringLiteral("%1 · %2").arg(span, size),
                  "trophy", 3000, 2); // critical: visible over screenshare
            }
            m_recordingLibrary->refresh();
          });
  connect(m_replayManager.get(), &ReplayManager::replayFailed, this,
          [this](const QString &reason) {
            m_notificationService->showNotification(
                "Couldn't Save Clip", reason, "dialog-warning", 3000, 2);
          });
  connect(m_settingsManager.get(), &SettingsManager::instantReplayEnabledChanged,
          this, [this]() {
            if (!m_settingsManager->instantReplayEnabled()) {
              m_replayManager->stopDvr();
            } else if (m_gameDetector &&
                       !m_gameDetector->currentGameName().isEmpty() &&
                       !m_recordingManager->isRecording()) {
              m_replayManager->startDvr(m_gameDetector->currentGameName());
            }
          });
  connect(m_settingsManager.get(),
          &SettingsManager::globalShortcutsEnabledChanged, this, [this]() {
            if (m_shortcutManager)
              m_shortcutManager->setEnabled(
                  m_settingsManager->globalShortcutsEnabled());
          });

  // --- Autostart: settings are the source of truth; the file follows ---
  connect(m_settingsManager.get(), &SettingsManager::startOnLoginChanged,
          this, [this]() { syncAutostartFile(); });
}

bool Application::isRecording() const { return m_recordingManager && m_recordingManager->isRecording(); }
QString Application::currentGameName() const { return m_gameDetector ? m_gameDetector->currentGameName() : QString(); }

bool Application::gameAudioNodePresent(const QString &game, qint64 pid) {
  // PW_ID_ANY without pulling pipewire headers into Application.
  constexpr uint32_t kNoNode = 0xFFFFFFFFu;
  if (pid > 0 &&
      PipeWireAudioSource::findNodeByPid(static_cast<uint32_t>(pid)) !=
          kNoNode) {
    return true;
  }
  if (!game.isEmpty() &&
      PipeWireAudioSource::findNodeByProcessName(game) != kNoNode) {
    return true;
  }
  return false;
}

void Application::startWhenAudioSettled(const QString &game, bool isRecord) {
  qint64 pid = 0;
  if (m_gameDetector && m_gameDetector->currentGameName() == game)
    pid = m_gameDetector->currentGamePid();
  if (gameAudioNodePresent(game, pid)) {
    qCDebug(appLog) << "Game audio present for" << game
                    << "- starting capture immediately";
    if (isRecord)
      m_recordingManager->startRecording(game);
    else
      m_replayManager->startDvr(game);
    return;
  }
  // Node not there yet (launching game): wait for it to appear and look
  // stable across two polls; the timeout starts anyway with today's
  // monitor-fallback behavior so a silent game never blocks capture.
  qCDebug(appLog) << "Deferring capture for" << game
                  << "until its audio node settles";
  m_pendingAudioGame = game;
  m_pendingAudioIsRecord = isRecord;
  m_audioWaitHits = 0;
  m_audioWaitTicks = 0;
  m_audioWaitTimer->start();
}

void Application::authorizeGame(const QString &game) {
  if (!m_gameDetector || game.isEmpty())
    return;
  // Prefer the detector's raw match key (reporter class / steam_app_<id>)
  // so future launches re-match; fall back to the display name (Steam
  // resolution is deterministic, so that re-matches too).
  QString key = game;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty())
    key = m_gameDetector->currentGameKey();
  if (m_gameDetector->addToWhitelist(key, game))
    qCInfo(appLog) << "Auto-whitelisted authorized game:" << game << "("
                   << key << ")";
}

void Application::blockGame(const QString &game) {
  if (!m_gameDetector || game.isEmpty())
    return;
  QString key = game;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty())
    key = m_gameDetector->currentGameKey();
  if (m_gameDetector->moveToBlacklist(key))
    qCInfo(appLog) << "Blocked game from authorize prompt:" << game << "("
                   << key << ")";
}

bool Application::isGameBlocked(const QString &game) const {
  if (!m_gameDetector || game.isEmpty())
    return false;
  if (m_gameDetector->currentGameName() == game &&
      !m_gameDetector->currentGameKey().isEmpty() &&
      m_gameDetector->isBlocked(m_gameDetector->currentGameKey()))
    return true;
  return m_gameDetector->isBlocked(game);
}

void Application::startWhenWindowReady(const QString &game, bool isRecord) {
  if (m_gameDetector && m_gameDetector->hasVisibleWindow()) {
    qCDebug(appLog) << "Window present for" << game
                    << "- waiting for settled game audio";
    startWhenAudioSettled(game, isRecord);
    return;
  }
  // The portal can only restore against a concrete window; requesting now
  // would pop a picker for a game that is still launching. Wait for the
  // window (timeout falls back to the authorize prompt, never a picker).
  qCDebug(appLog) << "Deferring capture for" << game
                  << "until its window appears";
  m_pendingWindowGame = game;
  m_pendingWindowIsRecord = isRecord;
  m_windowWaitTimer->start();
}
bool Application::isWayland() const { return CaptureFactory::detectDisplayServer() == CaptureFactory::DisplayServer::Wayland; }

bool Application::isEncoderAvailable(const QString &name) const {
  if (name == "auto" || name == "x264") return true;
  for (const auto &enc : EncoderFactory::detectAvailableEncoders()) { if (enc.name == name) return true; }
  return false;
}

bool Application::suppressNextStorageNotification() const {
  return m_suppressNextStorageNotification;
}

void Application::setSuppressNextStorageNotification(bool suppress) {
  if (m_suppressNextStorageNotification != suppress) {
    m_suppressNextStorageNotification = suppress;
    emit suppressNextStorageNotificationChanged();
  }
}

bool Application::isTrayAvailable() const {
  return m_trayManager && m_trayManager->isAvailable();
}

QObject *Application::qmlRootWindow() const {
  if (!m_engine || m_engine->rootObjects().isEmpty()) {
    return nullptr;
  }
  return m_engine->rootObjects().first();
}

void Application::setMainWindowVisible(bool visible) {
  QObject *root = qmlRootWindow();
  if (!root) {
    return;
  }
  root->setProperty("visible", visible);
  if (visible) {
    if (QWindow *win = qobject_cast<QWindow *>(root)) {
      win->show();
      win->raise();
      win->requestActivate();
    }
  }
  if (m_trayManager) {
    m_trayManager->setWindowVisible(visible);
  }
}

void Application::showMainWindow() { setMainWindowVisible(true); }

bool Application::minimizeToTray() {
  if (!isTrayAvailable() || !m_settingsManager->minimizeToTray()) {
    return false;
  }
  setMainWindowVisible(false);
  if (!m_minimizeNotifyShown) {
    m_minimizeNotifyShown = true;
    m_notificationService->showNotification(
        "Trophy minimized",
        m_recordingManager->isRecording()
            ? "Recording continues in the background. Right-click the tray icon to stop."
            : "Trophy keeps running in the system tray.",
        "trophy", 5000, 1);
  }
  return true;
}

void Application::requestQuit() {
  if (m_recordingManager && m_recordingManager->isRecording()) {
    // Bring the window up and let the QML confirm dialog decide.
    showMainWindow();
    if (QObject *root = qmlRootWindow()) {
      QMetaObject::invokeMethod(root, "requestQuit", Qt::QueuedConnection);
    }
    return;
  }
  QCoreApplication::quit();
}

void Application::playClipPing(bool preview) {
  playChime(QStringLiteral(":/sounds/clip-saved.wav"),
            QStringLiteral("clip-saved.wav"), preview);
}

void Application::playRecordingStoppedPing(bool preview) {
  playChime(QStringLiteral(":/sounds/recording-stopped.wav"),
            QStringLiteral("recording-stopped.wav"), preview);
}

void Application::playChime(const QString &resource, const QString &fileName,
                            bool preview) {
  if (!preview && (!m_settingsManager || !m_settingsManager->completionSoundEnabled()))
    return;
  // pw-play needs a real file: extract the bundled chime to cache once.
  const QString dest =
      QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
      QStringLiteral("/trophy/sounds/") + fileName;
  if (!QFile::exists(dest)) {
    QDir().mkpath(QFileInfo(dest).path());
    if (!QFile::copy(resource, dest)) {
      qCWarning(appLog) << "Could not extract chime" << resource << "to"
                        << dest;
      return;
    }
  }
  // Fire-and-forget: a missing/slow player must never hang the UI thread.
  if (!QProcess::startDetached(QStringLiteral("pw-play"),
                               QStringList(dest))) {
    qCWarning(appLog) << "Could not start pw-play for chime" << fileName;
  }
}

void Application::requestReplay() {
  if (!m_replayManager || !m_settingsManager) {
    return;
  }
  if (m_recordingManager && m_recordingManager->isRecording()) {
    m_notificationService->showNotification(
        "Already Recording",
        "You're already recording — the full session is being saved.",
        "trophy", 3000, 2);
    return;
  }
  if (!m_settingsManager->instantReplayEnabled()) {
    m_notificationService->showNotification(
        "Clips Are Off",
        "Clips are off. Enable them in Settings to buffer gameplay.",
        "trophy", 3000, 2);
    return;
  }
  if (!m_replayManager->isActive()) {
    m_notificationService->showNotification(
        "Nothing Buffered",
        "Launch a game first — there's nothing buffered yet.",
        "trophy", 3000, 2);
    return;
  }
  const int secs = std::max(1, m_settingsManager->clipBufferSeconds());
  m_notificationService->showNotification(
      "Saving Clip", QString("Saving the last %1 seconds…").arg(secs),
      "trophy", 3000, 2);
  m_replayManager->saveReplay();
}

void Application::setTrackMuted(int trackIndex, bool muted) {
  // Mute is a user preference, not pipeline state: set it on both mixers
  // so it holds whichever pipeline runs next. Rare UI action — logging it
  // traces dashboard mute clicks end to end.
  qCInfo(appLog) << "Track" << trackIndex << (muted ? "muted" : "unmuted");
  if (m_audioMixer)
    m_audioMixer->setTrackMuted(trackIndex, muted);
  if (m_replayManager)
    m_replayManager->setTrackMuted(trackIndex, muted);
}

bool Application::isTrackMuted(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->isTrackMuted(trackIndex);
  if (m_replayManager && m_replayManager->isActive())
    return m_replayManager->isTrackMuted(trackIndex);
  if (m_audioMixer)
    return m_audioMixer->isTrackMuted(trackIndex);
  return false;
}

float Application::trackLevel(int trackIndex) const {
  if (m_recordingManager && m_recordingManager->isRecording() && m_audioMixer)
    return m_audioMixer->getTrackLevel(trackIndex);
  if (m_replayManager && m_replayManager->isActive())
    return m_replayManager->trackLevel(trackIndex);
  return 0.0f;
}

void Application::requestToggleRecording() {  if (!m_recordingManager) {
    return;
  }
  if (m_recordingManager->isRecording()) {
    m_recordingManager->stopRecording();
    return;
  }
  const QString game =
      m_gameDetector ? m_gameDetector->currentGameName() : QString();
  if (!game.isEmpty()) {
    m_recordingManager->startRecording(game);
    return;
  }
  // Same as in-app F9 with no game, but a global keypress deserves
  // feedback instead of a silent no-op.
  m_notificationService->showNotification(
      "Trophy", "No game detected — open Trophy to record manually.",
      "trophy", 5000, 1);
}

void Application::syncAutostartFile() {
  if (!m_autostartManager || !m_settingsManager) {
    return;
  }
  const bool want = m_settingsManager->startOnLogin();
  if (want == m_autostartManager->isEnabled()) {
    return;
  }
  if (!m_autostartManager->setEnabled(want, QCoreApplication::applicationFilePath())) {
    qCWarning(appLog) << "Failed to sync autostart entry";
  }
}

bool Application::glShimAvailable() const {
  return !GlShimEnrollment::libraryPath().isEmpty();
}

bool Application::glShimEnrolled() const {
  return GlShimEnrollment::isEnrolled();
}

bool Application::glShimActive() const {
  return GlShimEnrollment::isActiveInCurrentSession();
}

bool Application::setGlShimEnrolled(bool enrolled) {
  const bool ok = GlShimEnrollment::setEnrolled(enrolled);
  emit detectionStatusChanged();
  return ok;
}

bool Application::setVulkanLayerEnabled(bool enabled) {
  m_settingsManager->setVulkanLayerEnabled(enabled);
  bool ok;
  if (enabled) {
    m_vulkanLayerRegistered = ensureVulkanLayerRegistered();
    ok = m_vulkanLayerRegistered;
  } else {
    ok = removeVulkanLayerRegistration();
    m_vulkanLayerRegistered = false;
  }
  emit detectionStatusChanged();
  return ok;
}

bool Application::removeVulkanLayerRegistration() {
  // Opt-out removes our JSON so the loader stops loading the layer.
  // Absence counts as success: nothing registered is the desired state.
  return VulkanLayer::removeRegistration();
}

bool Application::ensureVulkanLayerRegistered() {
  return VulkanLayer::ensureRegistered();
}

} // namespace TrophyBackend